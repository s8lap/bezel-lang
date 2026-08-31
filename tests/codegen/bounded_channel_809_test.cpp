// Codegen/runtime — §6.9 / §30.15 v0.174 (#809): BOUNDED channels + SendAsync.
//
// Derives from the v0.174 back-pressure revision (stdlib §6.9, spec §30.15's
// bounded paragraph):
//   - `new Channel<T>(capacity)` — capacity >= 1, else ArgumentException.
//   - `ChannelSender<T>.SendAsync(owned item): Task` awaits capacity: completes
//     immediately when a slot is free or the channel is unbounded; on a full
//     bounded channel it suspends and completes when a Receive frees a slot.
//   - Sync `Send` never blocks; on a FULL bounded channel it raises the new
//     `ChannelFullException`.
//   - Multiple pending SendAsyncs complete in FIFO order as capacity frees.
//   - Closing while a SendAsync waits disposes the pending item (class T) and
//     faults the send task with ChannelClosedException.
//
// These run the FULL front-end including the ownership pass (checkOwnership),
// because the send owned-sink transfer (§6.9) and channel item disposal depend
// on it — exactly like null_coalesce_owned_disposal_1029_test.cpp.  Programs are
// single-domain: a SendAsync that parks returns a pending Task synchronously
// (it never blocks the domain), so a later Receive on the same domain drains a
// slot and wakes it — no deadlock.

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
// exit code, or -999 on any front-end / JIT failure.
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
static const std::filesystem::path& wtDir809() {
    return wtScratchDir("wt-809");
}

// AOT ASan probe (mirrors async_618_domain_serial_test.cpp::runUnderSanitizer):
// emit an object from `source`, compile the runtime under ASan, link, run with
// leak detection.  Returns 0 (clean), 1 (ASan/LSan hit — leak/double-free), or
// -1 when clang++/the sub-compile is unavailable (infra skip, not a failure).
int runUnderAsan(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;
    auto dir = wtDir809();
    std::string stem = unique_stem("bezel_809_asan");
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
const char* kItem =
    "sealed class Item "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Item.disposed := Item.disposed + 1; end destructor; "
    "end Item; ";

}  // namespace

// §6.9 v0.174: fill a bounded channel to capacity, then the SYNC Send on the
// full channel raises ChannelFullException (a catchable, defined fault — the
// never-blocks discipline: a producer that wants to wait uses SendAsync).
BEZEL_TEST(bounded_channel_809, sync_send_full_raises_channel_full_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(2); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(1); "
        "    tx.Send(2); "                       // buffer now full (capacity 2)
        "    var caught := 0; "
        "    try "
        "      tx.Send(3); "                     // full → ChannelFullException
        "    catch ChannelFullException => do caught := 1; end "
        "    end; "
        "    return caught; "                    // 1
        "  end Main; "
        "end C;"), 1);
}

// §6.9 v0.174: SendAsync on a full bounded channel SUSPENDS, then completes once
// a Receive frees a slot; the queued value is delivered in order.
BEZEL_TEST(bounded_channel_809, send_async_suspends_then_completes_after_receive) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(1); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(10); "                       // capacity 1 → full
        "    var pending := tx.SendAsync(20); "   // parks (full); returns a pending Task
        "    var a := await ch.Receive(); "       // 10 out → admit 20, wake the send
        "    await pending; "                      // now completed
        "    var b := await ch.Receive(); "       // 20 out
        "    return a + b; "                       // 30
        "  end Main; "
        "end C;"), 30);
}

// §6.9 v0.174: multiple pending SendAsyncs complete in FIFO order as slots free.
// Receiving 1,2,3 in that exact order pins the send-waiter FIFO (a LIFO or
// unordered queue would surface 2 and 3 swapped → not 123).
BEZEL_TEST(bounded_channel_809, pending_senders_complete_fifo) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(1); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(1); "                         // buffer=[1] full
        "    var s2 := tx.SendAsync(2); "          // park sender #1 (item 2)
        "    var s3 := tx.SendAsync(3); "          // park sender #2 (item 3)
        "    var a := await ch.Receive(); "        // 1 out → admit 2
        "    await s2; "
        "    var b := await ch.Receive(); "        // 2 out → admit 3
        "    await s3; "
        "    var c := await ch.Receive(); "        // 3 out
        "    return a * 100 + b * 10 + c; "        // 123
        "  end Main; "
        "end C;"), 123);
}

// §6.9 v0.174: closing while a SendAsync waits faults the send task with
// ChannelClosedException AND disposes the pending item exactly once (class T).
// Item.disposed counts destructor runs: item0 (buffered) + item1 (pending) = 2,
// neither ever received.  The `owned` call-site markers (§22.3, v0.169) exercise
// the SendAsync owned-sink transfer added for #809 — the named bindings move into
// the channel, so codegen does NOT scope-end-dispose them (the channel owns them).
BEZEL_TEST(bounded_channel_809, close_while_waiting_faults_and_disposes_item_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var ch := new Channel<Item>(1); "
        "    var tx := ch.NewSender(); "
        "    var it0 := new Item(0); "
        "    tx.Send(owned it0); "                 // buffer=[it0] full (owned move)
        "    var it1 := new Item(1); "
        "    var s := tx.SendAsync(owned it1); "   // parks it1 (full; owned move)
        "    var faulted := 0; "
        "    tx.Dispose(); "                       // last sender → close+drain: dispose it1, fault s
        "    try "
        "      await s; "                          // faulted with ChannelClosedException
        "    catch ChannelClosedException => do faulted := 1; end "
        "    end; "
        "    ch.Dispose(); "                       // disposes the buffered it0
        "    return faulted * 100 + Item.disposed; "  // 1*100 + 2 = 102
        "  end Main; "
        "end C;"), 102);
}

// §6.9 v0.174: SendAsync on an UNBOUNDED channel completes immediately (awaiting
// it never suspends), so a straight produce-then-consume yields both values.
BEZEL_TEST(bounded_channel_809, unbounded_send_async_completes_immediately) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "   // unbounded (nullary ctor)
        "    using var tx := ch.NewSender(); "
        "    await tx.SendAsync(5); "                 // immediate (unbounded)
        "    await tx.SendAsync(6); "                 // immediate
        "    var a := await ch.Receive(); "
        "    var b := await ch.Receive(); "
        "    return a + b; "                          // 11
        "  end Main; "
        "end C;"), 11);
}

// §6.9 v0.174: `new Channel<T>(0)` — capacity must be >= 1, else a catchable
// ArgumentException (the guard fires before the channel is allocated).
BEZEL_TEST(bounded_channel_809, capacity_zero_raises_argument_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var caught := 0; "
        "    try "
        "      using var ch := new Channel<int>(0); "
        "    catch ArgumentException => do caught := 1; end "
        "    end; "
        "    return caught; "                        // 1
        "  end Main; "
        "end C;"), 1);
}

// §6.9 v0.174: a NEGATIVE capacity is equally rejected with ArgumentException.
BEZEL_TEST(bounded_channel_809, capacity_negative_raises_argument_exception) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var caught := 0; "
        "    try "
        "      using var ch := new Channel<int>(-1); "
        "    catch ArgumentException => do caught := 1; end "
        "    end; "
        "    return caught; "                        // 1
        "  end Main; "
        "end C;"), 1);
}

// §6.9 v0.174: Count reports only BUFFERED items — a pending SendAsync (parked,
// not yet admitted to the buffer) is NOT counted.  capacity 1: Count is 1 while
// item 2 is parked, and still 1 after a Receive admits item 2.
BEZEL_TEST(bounded_channel_809, count_excludes_pending_senders) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(1); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(1); "                           // buffer=[1] full
        "    var s := tx.SendAsync(2); "             // parks (not buffered)
        "    var cnt := ch.Count; "                  // 1 — pending sender excluded
        "    var a := await ch.Receive(); "          // 1 out → admit 2
        "    await s; "
        "    var cnt2 := ch.Count; "                 // 1 — item 2 now buffered
        "    var b := await ch.Receive(); "          // 2 out
        "    return cnt * 100 + cnt2 * 10 + (a + b); "  // 1*100 + 1*10 + 3 = 113
        "  end Main; "
        "end C;"), 113);
}

// §6.9 v0.174: class-T SendAsync on an unbounded channel — immediate accept,
// then received and disposed exactly once (value delivered, no leak/double-free).
BEZEL_TEST(bounded_channel_809, class_send_async_unbounded_deliver_and_dispose) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "  // unbounded
        "    using var tx := ch.NewSender(); "
        "    await tx.SendAsync(new Item(7)); "       // immediate
        "    var got := await ch.Receive(); "         // owns the Item
        "    var v := got.V; "
        "    got.Dispose(); "                         // dispose exactly once
        "    return v * 10 + Item.disposed; "         // 7*10 + 1 = 71
        "  end Main; "
        "end C;"), 71);
}

// --- ASan AOT probes: no leak, no double-free on the two owned-item paths ---

// §6.9 v0.174 ASan: close-while-waiting disposal path.  A class-T item parked in
// a SendAsync is disposed exactly once by the channel close; the buffered item is
// disposed by Dispose.  ASan/LSan must report NO leak and NO double-free.
BEZEL_TEST(bounded_channel_809, asan_close_while_waiting_disposal_no_leak) {
    int r = runUnderAsan(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    var ch := new Channel<Item>(1); "
        "    var tx := ch.NewSender(); "
        "    var it0 := new Item(0); "
        "    tx.Send(owned it0); "
        "    var it1 := new Item(1); "
        "    var s := tx.SendAsync(owned it1); "     // parks it1
        "    tx.Dispose(); "                          // close+drain: dispose it1, fault s
        "    try await s; catch ChannelClosedException => do end end; "
        "    ch.Dispose(); "                          // dispose buffered it0
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r != -1) BEZEL_EXPECT_EQ(r, 0);
}

// §6.9 v0.174 ASan: a full bounded produce/consume lifecycle.  A producer fills
// capacity, back-pressures via SendAsync, and the consumer drains everything so
// each Item is delivered and disposed exactly once — ASan/LSan clean.
BEZEL_TEST(bounded_channel_809, asan_full_bounded_produce_consume_lifecycle) {
    int r = runUnderAsan(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(2); "  // bounded capacity 2
        "    using var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "                  // buffer=[1]
        "    tx.Send(new Item(2)); "                  // buffer=[1,2] full
        "    var s := tx.SendAsync(new Item(3)); "    // parks (full)
        "    using var a := await ch.Receive(); "     // 1 out → admit 3
        "    await s; "
        "    using var b := await ch.Receive(); "     // 2 out
        "    using var c := await ch.Receive(); "     // 3 out
        "    return a.V + b.V + c.V; "                 // received + disposed once each
        "  end Main; "
        "end C;");
    if (r != -1) BEZEL_EXPECT_EQ(r, 0);
}
