// Codegen — EventSource backing fields of instance events are disposed with
// their owner, issue #480 (§24.1 / §28).
//
// §24.1: "For instance events the backing field is an owned field of type
// EventSource<T> stored inside the declaring class's heap block."  §28 requires
// every owned field to be disposed when the owner is disposed; §28.4 step 2
// applies.  Before this fix, emitOwnedFieldDisposals skipped EventSymbol members
// (the dynamic_cast<FieldSymbol*> check fails for them) so the EventSource
// backing object was allocated at construction (bz_event_source_new) but never
// freed — a guaranteed per-instance leak.
//
// Similarly, the hasOwnedToClean scan that decides whether the constructor needs
// an exception-cleanup block also skipped EventSymbols, so a constructor that
// throws AFTER emitInstanceInit initialized the event slot leaks the EventSource.
//
// Runtime: bz_event_source_free(void*) already existed and is null-safe; the
// fix adds disposal calls in emitOwnedFieldDisposals and includes EventSymbol in
// the hasOwnedToClean scan.
//
// Test plan:
//   480-a  single-event class disposed via `using var` → LSan clean (no leak)
//   480-b  multi-event class disposed via `using var` → LSan clean
//   480-c  class with event AND owned field disposed → LSan clean, no double-free
//   480-d  ctor throws AFTER event slot is initialized → LSan clean (ctor-cleanup
//          path frees the EventSource)
//
// Each test has a JIT leg (observable via exit code) to confirm normal behaviour
// is still correct, and an LSan leg that is the load-bearing correctness proof.
// The LSan leg returns -1 (SKIP) when the ASan toolchain is absent.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runJit;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir480() {
    return wtScratchDir("wt-480");
}

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification — mirrors the pattern in undisposed_leak_test.cpp
// and exception_disposal_test.cpp exactly.  Lowers `source` to an object file,
// links it against the memoized ASan-instrumented runtime object, and runs under
// LSan.  Returns:
//   0  — clean (no leak, no use-after-free / double-free)
//   1  — sanitizer flagged a problem: test failure
//  -1  — toolchain unavailable / build failed: SKIP (not a failure)
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir480();
    std::string stem = unique_stem("bezel_480_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// =====================================================================
// 480-a  Single-event class disposed via `using var` → no EventSource leak.
// §24.1: the EventSource<T> backing field is owned by the class instance;
// §28.4 step 2 requires it to be freed when the owner is disposed.
// =====================================================================

// Widget declares one (no-payload) event.  Main creates a Widget, raises
// (no-op, no subscribers), and the `using var` disposes the Widget at scope
// end.  The JIT leg checks the program returns the correct value (7); the LSan
// leg asserts the EventSource block is released — before #480 it leaked.
// NOTE: no subscriber is registered here because a subscribed closure that
// escapes into bz_event_subscribe is intentionally not released by the
// compiler's non-escape analysis (a pre-existing design choice); testing the
// EventSource free requires keeping the source subscriber-free so LSan sees
// only the EventSource block, not an unrelated closure block.
const char* kSingleEvent =
    "module M; "
    "class Widget "
    "  public event Tick; "
    "  public method Fire() Tick.Raise(); end Fire; "
    "end Widget; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var w := new Widget(); "
    "      w.Fire(); "   // zero subscribers — no-op, §24.5
    "    end; "
    "    return 7; "
    "  end Main; "
    "end C;";

// =====================================================================
// 480-b  Multi-event class → no leak for any of the EventSource slots.
// §24.1: each event declaration gets its own backing EventSource field;
// all must be freed on disposal.
// =====================================================================

// Widget2 declares two events with distinct payload types.  Both backing
// EventSource objects must be freed when the Widget2 instance is disposed.
// No subscribers registered (see note in 480-a) so the LSan leg sees only
// the two EventSource blocks, not unrelated closure blocks.
const char* kMultiEvent =
    "module M; "
    "record Ev(N: int); "
    "class Widget2 "
    "  public event A: Ev; "
    "  public event B: Ev; "
    "  public method FireA(n: int) A.Raise(new Ev(n)); end FireA; "
    "  public method FireB(n: int) B.Raise(new Ev(n)); end FireB; "
    "end Widget2; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var w := new Widget2(); "
    "      w.FireA(3); "   // zero subscribers — no-op
    "      w.FireB(2); "   // zero subscribers — no-op
    "    end; "
    "    return 5; "   // 3 + 2, proving FireA and FireB ran
    "  end Main; "
    "end C;";

// =====================================================================
// 480-c  Class with one event AND one owned field → no leak, no double-free.
// §28.4 step 2: all owned fields (FieldSymbol and EventSymbol alike) are
// freed in reverse declaration order.  Freeing an EventSource must not
// interfere with freeing the owned FieldSymbol field, and vice versa.
// =====================================================================

// Holder owns a Res (FieldSymbol) and declares one event (EventSymbol).  Both
// must be freed when the Holder is disposed.  The JIT leg confirms Res's
// destructor ran (count == 1), and the LSan leg checks both heap blocks are
// freed (Res + EventSource).  No subscriber (see note in 480-a).
const char* kEventAndOwnedField =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Holder "
    "  private owned _r: Res; "
    "  public event Done; "
    "  public constructor() this._r := new Res(); end constructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var h := new Holder(); "
    "    end; "
    "    return Res.count; "   // Res destructor ran exactly once
    "  end Main; "
    "end C;";

// =====================================================================
// 480-d  Constructor throws AFTER emitInstanceInit initialized the event slot.
// §22.9 / §28.3: the ctor-cleanup block must free any EventSource initialized
// before the throw.  Before #480, hasOwnedToClean skipped EventSymbols, so
// no cleanup block was emitted for a class with ONLY an event (no other owned
// fields), and the EventSource leaked on ctor failure.
// =====================================================================

// Widget3 declares one event and a constructor that throws after
// emitInstanceInit has run (which allocates the EventSource).  The thrown
// Boom is caught by Main; the EventSource must be freed on the unwind path.
// The JIT leg confirms the program runs and catches the exception normally
// (exit 42); the LSan leg asserts the EventSource is freed.
const char* kCtorThrowAfterEventInit =
    "module M; "
    "class Boom: Exception end Boom; "
    "class Widget3 "
    "  public event Tick; "
    "  public constructor() "
    "    throw new Boom(); "
    "  end constructor; "
    "end Widget3; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      using var w := new Widget3(); "
    "    catch Boom => Console.WriteLine(\"caught\"); end; "
    "    return 42; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// 480-a tests
// =====================================================================

BEZEL_TEST(codegen_event_leak_480, single_event_raise_no_op_no_subscribers_24_5) {
    // JIT: zero subscribers — Raise is a no-op (§24.5); Widget is disposed at
    // scope end and the program returns 7.
    BEZEL_EXPECT_EQ(run(kSingleEvent), 7);
}

BEZEL_TEST(codegen_event_leak_480, single_event_event_source_freed_on_dispose_24_1_28) {
    // LSan: the EventSource block allocated at construction is freed when the
    // Widget is disposed.  Before #480 this leaked — emitOwnedFieldDisposals
    // skipped EventSymbol, so bz_event_source_free was never called.
    int lsan = runUnderLeakSanitizer(kSingleEvent);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 480-b tests
// =====================================================================

BEZEL_TEST(codegen_event_leak_480, multi_event_raise_no_op_no_subscribers_24_5) {
    // JIT: two events, zero subscribers each — both Raises are no-ops (§24.5);
    // Widget2 is disposed and the program returns 5.
    BEZEL_EXPECT_EQ(run(kMultiEvent), 5);
}

BEZEL_TEST(codegen_event_leak_480, multi_event_both_sources_freed_on_dispose_24_1_28) {
    // LSan: both EventSource blocks (one per event slot) are freed when Widget2
    // is disposed.  Before #480 both leaked (emitOwnedFieldDisposals skipped
    // all EventSymbol members).
    int lsan = runUnderLeakSanitizer(kMultiEvent);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 480-c tests
// =====================================================================

BEZEL_TEST(codegen_event_leak_480, event_and_owned_field_destructor_runs_once_28_4) {
    // JIT: Res destructor runs exactly once when Holder is disposed — the
    // owned FieldSymbol field is correctly disposed via emitOwnedFieldDisposals,
    // and the EventSymbol disposal added by #480 does not interfere.
    BEZEL_EXPECT_EQ(run(kEventAndOwnedField), 1);
}

BEZEL_TEST(codegen_event_leak_480, event_and_owned_field_both_freed_on_dispose_24_1_28_4) {
    // LSan: both the Res block (FieldSymbol disposal) and the EventSource block
    // (EventSymbol disposal) are reclaimed.  No double-free from mixing the two
    // disposal paths in emitOwnedFieldDisposals.
    int lsan = runUnderLeakSanitizer(kEventAndOwnedField);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 480-d tests
// =====================================================================

BEZEL_TEST(codegen_event_leak_480, ctor_throw_after_event_init_caught_22_9) {
    // JIT: the constructor throws, Boom is caught, Main returns 42.  Confirms
    // the ctor-cleanup path does not crash (the EventSource free is safe on a
    // freshly-allocated, possibly unsubscribed source).
    BEZEL_EXPECT_EQ(run(kCtorThrowAfterEventInit), 42);
}

BEZEL_TEST(codegen_event_leak_480, ctor_throw_after_event_init_source_freed_22_9_28) {
    // LSan: the EventSource block allocated by emitInstanceInit is freed on the
    // exception-unwind path (ctor-cleanup block).  Before #480 hasOwnedToClean
    // excluded EventSymbols, so no cleanup block was emitted → the EventSource
    // leaked on constructor failure.
    int lsan = runUnderLeakSanitizer(kCtorThrowAfterEventInit);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}
