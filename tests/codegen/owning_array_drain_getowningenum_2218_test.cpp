// #2218 / stdlib §4.5/§4.13 / spec §29.1/§29.2 v0.195: OwningArray<T>.Drain() /
// OwningSet<T> / OwningQueue<T> implement IOwningEnumerable<T> at the bz_array_t
// level — the drain view IS the receiver collection, with no separate
// enumerable object. Calling GetOwningEnumerator() on the drain view must
// therefore build a vtable-backed IOwningEnumerator<T> wrapper at the call
// site, not dispatch slot 4 of a bz_array_t* (which has no vtable — the first
// field is `data`, and reading it as a vtable pointer + GEP-slot-4 produces
// a SEGV; #2218 / #1794).
//
// The wrapper, keyed on the element type and shared per compilation unit:
//   * MoveNext (slot 2) pops the next element via bz_array_drain_next.
//   * TakeCurrent (slot 3) moves the pending element out (slot nulled).
//   * Dispose (slot 1) disposes the advanced-but-untaken pending value
//     (§29.1 manual-protocol abandonment), then frees the wrapper. The
//     backing array is NOT freed (the source `a` still owns it).
//
// These tests verify both the value path (MoveNext yields the expected
// elements in FIFO order) and the memory-safety path (ASan/LSan clean — no
// SEGV, no leak, no double-free of the advanced-but-untaken element).
//
// Spec citations: §29.1 (TakeCurrent transfers ownership, Dispose disposes
// advanced-but-untaken pending value), §29.2 (desugaring of for-in over
// IOwningEnumerable<T>), §29.7 (Dispose of partially-consumed iterators),
// stdlib §4.5/§4.10/§4.11 v0.135 (Drain() returns the receiver at
// IOwningEnumerable<T> static type).

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
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {
namespace fs = std::filesystem;

// AOT compile → native exec (no ASan). Returns the program's exit code, or
// -999 when the AOT pipeline cannot be completed in this environment.
int runAotExitCode(const std::string& source) {
    auto r = runAot(source);
    return r.ran ? r.exitCode : -999;
}

// AOT compile → object → link with ASan → run. Returns 0 (clean), 1
// (sanitizer fired), -1 (toolchain missing).
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_2218_asan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
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
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

}  // namespace

// ===========================================================================
// §29.1/§29.2 / #2218: OwningArray<T>.Drain().GetOwningEnumerator() manual
// protocol — no SEGV (was the pre-fix crash). The drain view IS the array
// (no separate enumerable); the wrapper built at the call site dispatches
// MoveNext via bz_array_drain_next, returning the element via TakeCurrent.
// ===========================================================================

// Single MoveNext → TakeCurrent yields the first element (FIFO, §4.5).
// `a` is the source OwningArray<C> (two C instances); MoveNext pops index 0;
// TakeCurrent transfers it. Return 1 if the element's Id is 1 (the first
// pushed). The second C is still in the array; `a.Dispose` at scope end
// reclaims it.
BEZEL_TEST(owning_array_drain_getowningenum_2218,
           manual_protocol_single_yield_returns_first_spec_29_1) {
    int rc = runAotExitCode(
        "module M; "
        "class C "
        "  public Id: int := 0; "
        "  public constructor(n: int) Id := n; end constructor; "
        "end C; "
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<C>(); "
        "    a.Add(new C(1)); "
        "    a.Add(new C(2)); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      using var u := en.TakeCurrent(); "
        "      return u.Id; "
        "    end; "
        "    return -1; "
        "  end Main; end T;");
    if (rc == -999) return;  // AOT unavailable
    BEZEL_EXPECT_EQ(rc, 1);
}

// Advance past every element; for-in desugaring via GetOwningEnumerator +
// TakeCurrent transfers each in FIFO order. Result: sum of Ids = 1+2+3 = 6.
BEZEL_TEST(owning_array_drain_getowningenum_2218,
           manual_protocol_drain_all_fifo_sum_spec_29_2) {
    int rc = runAotExitCode(
        "module M; "
        "class C "
        "  public Id: int := 0; "
        "  public constructor(n: int) Id := n; end constructor; "
        "end C; "
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<C>(); "
        "    a.Add(new C(1)); "
        "    a.Add(new C(2)); "
        "    a.Add(new C(3)); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    var s := 0; "
        "    while en.MoveNext() do "
        "      using var u := en.TakeCurrent(); "
        "      s := s + u.Id; "
        "    end; "
        "    return s; "
        "  end Main; end T;");
    if (rc == -999) return;
    BEZEL_EXPECT_EQ(rc, 6);
}

// ===========================================================================
// §29.1 / #2218: manual-protocol abandonment — MoveNext + no TakeCurrent +
// Dispose. The advanced-but-untaken element MUST be disposed exactly once
// (§29.1 "advanced-but-untaken pending value"; §28.2 dispose-exactly-once).
//
// #2264: the original test sources embedded Bezel `//` line comments inside
// adjacent-string-literal concatenations that had NO newlines, so per §1.5
// the `//` ran to end of input — every `//` line consumed the rest of the
// program (the trailing `end;`, `return C.disposeCount;`, `end Main; end T;`).
// runUnderASan then returned -1 (parse failure) and the original
// `if (rc == -1) return;` silently passed the test. The cleaned-up sources
// below use `/* ... */` block comments (Bezel §1.5 — comments that DO have
// well-defined end) so the program is well-formed and the sentinel is now
// BEZEL_EXPECT_TRUE(rc != -1) so a regression in the toolchain surfaces as a
// hard test failure, not a silent skip.
// ===========================================================================

BEZEL_TEST(owning_array_drain_getowningenum_2218,
           asan_manual_protocol_untaken_pending_disposed_spec_29_1) {
    // The program returns 42 (a fixed sentinel) so the value-path assertion
    // is independent of the disposeCount. The ASan-path assertion checks
    // that no memory error fires. Before the vacuous-comment cleanup the
    // parse failed and both assertions were silently skipped; the program
    // is now well-formed (`/* ... */` block comments replace the broken
    // `// ...` line comments that ate the rest of the single-line source).
    const std::string src =
        "module M; "
        "class C "
        "  public static disposeCount: int := 0; "
        "  public constructor() C.disposeCount := 0; end constructor; "
        "  destructor C.disposeCount := C.disposeCount + 1; end destructor; "
        "end C; "
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<C>(); "
        "    a.Add(new C()); "
        "    a.Add(new C()); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      /* Intentionally DO NOT call TakeCurrent — the pending C is "
        "         the advanced-but-untaken value. Dispose must reclaim it. */ "
        "    end; "
        "    return 42; "
        "  end Main; end T;";
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);  // AOT unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcValue, 42);       // program completed normally

    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);     // toolchain unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcAsan, 0);          // 0 = ASan clean
}

// Same shape but with multiple MoveNexts and an advance-without-take midway.
// The wrapper's MoveNext disposes the previous pending (per §29.1 next-MoveNext
// contract) before popping the next; the array's last element is reclaimed
// by `a.Dispose` at scope end. Disposal count = N (every C runs its dtor
// exactly once — no double-free, no leak).
//
// #2264: also exercises the fixed-MoveNext path — the final (false) MoveNext
// MUST publish null to `pending` (not the stale popped pointer from the
// previous true-MoveNext), or the using-var-en scope-end Dispose would
// emitDisposeAndReclaim on freed memory.
BEZEL_TEST(owning_array_drain_getowningenum_2218,
           asan_manual_protocol_full_drain_no_leak_spec_29_1) {
    const std::string src =
        "module M; "
        "class C "
        "  public static disposeCount: int := 0; "
        "  public constructor() C.disposeCount := 0; end constructor; "
        "  destructor C.disposeCount := C.disposeCount + 1; end destructor; "
        "end C; "
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<C>(); "
        "    a.Add(new C()); "
        "    a.Add(new C()); "
        "    a.Add(new C()); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      /* Skip TakeCurrent — the wrapper's MoveNext on the next iter "
        "         disposes the prior pending (defensive §29.1 contract). */ "
        "    end; "
        "    return 42; "
        "  end Main; end T;";
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);  // AOT unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcValue, 42);       // program completed normally

    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);     // toolchain unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcAsan, 0);          // 0 = ASan clean
}

// ===========================================================================
// §29.1 / #2264: manual-protocol exhaustion — every element is taken via
// TakeCurrent, the final (false) MoveNext MUST leave `pending` null, and the
// using-var-en scope-end Dispose MUST be a no-op. Before the fix, the final
// (false) MoveNext's outSlot alloca was uninitialised; the load picked up
// the stale popped pointer from the previous true-MoveNext and stored it
// into `pending`, so Dispose called emitDisposeAndReclaim on freed memory
// (UAF; ASan flags the read-after-free in the runtime `free`/reclaim path).
//
// Asserts both the ASan-clean path AND the exact dispose count (via
// runAotExitCode, which returns the program's 8-bit exit code) — a stale
// pointer that happens to slip past ASan would still show up as an off-by-one
// disposeCount (e.g. 3 disposes for 2 elements ⇒ the UAF double-freed the
// last element). The two checks are independent: runAotExitCode runs WITHOUT
// generated-code ASan (matching the #2218 base path) and asserts the exit
// code; runUnderASan runs WITH ASan and asserts the exit-status-or-error
// envelope. If the toolchain is missing both fail HARD, no silent skip.
// ===========================================================================

BEZEL_TEST(owning_array_drain_getowningenum_2218,
           final_movenext_pending_is_null_no_uaf_spec_29_1) {
    const std::string src =
        "module M; "
        "class C "
        "  public static disposeCount: int := 0; "
        "  public constructor() C.disposeCount := 0; end constructor; "
        "  destructor C.disposeCount := C.disposeCount + 1; end destructor; "
        "end C; "
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<C>(); "
        "    a.Add(new C()); "
        "    a.Add(new C()); "
        "    C.disposeCount := 0; "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    while en.MoveNext() do "
        "      using var u := en.TakeCurrent(); "
        "    end; "
        "    return C.disposeCount; "
        "  end Main; end T;";

    // Value path: exactly 2 elements, exactly 2 disposes (one per `using var u`
    // scope-end). A stale-pending UAF that re-disposed the last element would
    // surface as 3; a leaked element (Dispose skipped because pending was
    // non-null and the using-var-en scope-end crashed) would surface as 1.
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);  // AOT unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcValue, 2);        // exactly-once disposal via live counter

    // ASan path: no UAF, no double-free, no leak.
    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);    // toolchain unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcAsan, 0);         // 0 = ASan clean
}