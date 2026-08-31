// #1794 / spec §29.1/§29.2, v0.282: an owning enumerator owns the advanced-but-
// untaken pending value; Dispose() disposes it, under BOTH §29.1 storage
// strategies. Pinning test: manual protocol over OwningArray<C>.Drain() —
// MoveNext(), no TakeCurrent(), dispose enumerator; assert the element's
// destructor ran exactly once (fails-without-fix if current impl leaks it or
// double-disposes via the collection).
//
// #2264 follow-up: also covers the MoveNext-after-MoveNext case (the §29.1
// "next MoveNext disposes previous untaken pending" defensive contract — the
// wrapper must dispose the previously-untaken value before popping the next,
// else the slot holds stale data on the final (false) MoveNext). The wrapper
// must also zero-init the outSlot alloca (already fixed in #2264) so the
// final (false) MoveNext publishes null to pending and the using-var-en
// scope-end Dispose is a no-op.
//
// The #2218 test (owning_array_drain_getowningenum_2218_test.cpp) covers the
// same shapes but only asserts ASan-clean; the disposeCount assertion is the
// additional signal this test pins (a leak that happened to slip past LSan,
// or a double-free that was only occasionally observable, would show up as
// an off-by-one in the dispose count).
//
// Spec citations: §29.1 v0.193/§29.2 v0.195 (ruled), stdlib §4.5/§4.13.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
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
    std::string stem = "bezel_1794_asan_" + std::to_string(counter++);
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

// Shared counting class for disposal tracking — destructor bumps the static
// counter so the test program can return the count via its exit code (8-bit).
const std::string kCountR =
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() R._n := R._n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// ===========================================================================
// §29.1 / #1794: manual-protocol abandonment — single MoveNext, no
// TakeCurrent, enumerator scope-end. The advanced-but-untaken pending value
// MUST be disposed exactly once: the wrapper's Dispose takes ownership of the
// pending value (per §29.1 "a Dispose() after an advance whose value was never
// taken disposes the pending value"), and the array's own Dispose at scope
// end reclaims only the remaining elements (the popped one is no longer in
// the array — bz_array_drain_next removes the slot).
//
// Program: 3 elements added to OwningArray; Drain().GetOwningEnumerator();
// single MoveNext; no TakeCurrent; `en` goes out of scope (wrapper disposes
// the pending first element); `a` goes out of scope (array disposes the
// remaining 2 elements). Total disposes = 3 (every C's destructor runs
// exactly once).
//
// Failure modes (pre-#2218 / pre-#2264):
//   * Leak: wrapper.Dispose doesn't touch pending → 2 disposes (last element
//     disposed twice is also a possible outcome if the wrapper's MoveNext
//     pops the slot but never clears the array's reference — but the current
//     bz_array_drain_next does call remove_at so the array's count is 2).
//   * Double-dispose: pending disposed both by wrapper AND by array (would
//     require the array to still hold a stale reference to the popped
//     element, which the current bz_array_drain_next also prevents).
// ===========================================================================

BEZEL_TEST(owning_enumerator_pending_value_1794,
           manual_protocol_untaken_pending_disposed_exactly_once_spec_29_1) {
    // The Drive sub-method holds the using-vars; the using-var disposals
    // fire at Drive's scope end BEFORE Main reads R._n. The destructors of
    // R only run during those disposals, so returning the counter from
    // Main directly (before the using-var scope ends) would always read
    // 0. This is the standard "exactly-once oracle" pattern tests/codegen/
    // owning_iter_exhaustion_abandon_leak_2616_test.cpp uses.
    const std::string src =
        "module M; " + kCountR +
        "class T "
        "  private static method Drive(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      /* Intentionally DO NOT call TakeCurrent — the pending R is "
        "         the advanced-but-untaken value. Dispose must reclaim it "
        "         exactly once (§29.1 v0.193). */ "
        "    end; "
        "    return R._n; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n + t; "
        "  end Main; end T;";

    // Value path: exactly 3 disposes (1 from wrapper.Dispose on the
    // untaken-pending, 2 from the array's Dispose on the remaining 2
    // elements). A leak would surface as 2; a double-dispose on the
    // popped element would surface as 4.
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);  // AOT unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcValue, 3);         // exactly-once disposal via counter

    // ASan path: no leak (LSan would fire on the leaked pending) and no
    // UAF/double-free (ASan would fire on the read-after-free).
    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);     // toolchain unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcAsan, 0);          // 0 = ASan clean
}

// ===========================================================================
// §29.1 / #1794: multi-MoveNext without any TakeCurrent — the wrapper's
// §29.1 "next-MoveNext disposes previous untaken pending" defensive contract
// (#747 / Iterators.cpp MoveNext pattern) MUST fire on the OWNING wrapper's
// MoveNext too, else the slot holds a stale reference and the next drain
// poisons it. Every element is disposed exactly once: each MoveNext between
// 1..N disposes the previous pending (i.e., 1 dispose per MoveNext), and
// the array's Dispose at scope end reclaims the remaining 0 elements.
//
// Program: 2 elements. First MoveNext pops element 0 (no TakeCurrent).
// Second MoveNext disposes element 0 (the pending from the previous advance)
// and pops element 1 (no TakeCurrent). `en` scope-end wrapper Dispose
// disposes element 1 (the latest pending). `a` scope-end reclaims the
// remaining 0 elements. Total = 2 disposes.
//
// Failure mode: if the wrapper's MoveNext does NOT dispose the previous
// pending before popping the next, element 0 leaks → only 1 dispose.
// ===========================================================================

BEZEL_TEST(owning_enumerator_pending_value_1794,
           manual_protocol_multi_movenext_each_disposed_exactly_once_spec_29_1) {
    // This test does NOT need a Drive sub-method wrap — the
    // while-en.MoveNext loop's body disposes each previous pending
    // BEFORE returning, so R._n is already correct at the return
    // statement. Only tests where the LAST MoveNext leaves a non-null
    // pending at return (and the using-var-en scope end is what
    // reclaims it) need the wrap.
    const std::string src =
        "module M; " + kCountR +
        "class T "
        "  public static method Main(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    /* Skip TakeCurrent on every iteration — the wrapper's MoveNext "
        "       must dispose the previous pending (the §29.1 next-MoveNext "
        "       defensive contract). */ "
        "    while en.MoveNext() do "
        "    end; "
        "    return R._n; "
        "  end Main; end T;";

    // Value path: 2 elements, both disposed by the wrapper's per-MoveNext
    // pending disposal (the LAST MoveNext returning false leaves pending=null
    // per #2264, so `en`'s using-var scope-end Dispose is a no-op). The
    // array's own Dispose at scope end finds 0 elements. Total = 2.
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);  // AOT unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcValue, 2);         // exactly-once disposal via counter

    // ASan path: defensive MoveNext-disposes-previous contract; without it
    // ASan would detect the stale-pointer UAF on the final (false) MoveNext
    // (#2264's zero-init fix is the matching end).
    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);     // toolchain unavailable -> HARD failure
    BEZEL_EXPECT_EQ(rcAsan, 0);          // 0 = ASan clean
}

// ===========================================================================
// §29.1 / #1794: mixed path — MoveNext + TakeCurrent + MoveNext + abandon.
// First iteration: MoveNext pops element 0 into pending, TakeCurrent
// transfers it into `u`, the inner `using var u` discharges at body end →
// 1 dispose. Second iteration: MoveNext pops element 1 into pending
// (NO TakeCurrent), `if` body falls through. After Drive returns the
// using-vars fire: `en`'s Dispose disposes pending element 1 → 1 more
// dispose; `a`'s Dispose disposes the remaining 0 elements. Total = 2.
//
// Failure mode: if the wrapper's Dispose doesn't reclaim the pending after
// the loop's fall-through (because the user never called TakeCurrent on the
// last advance), element 1 leaks → 1 dispose total. The TakeCurrent path
// on the first iteration is exercised too — a bug where TakeCurrent
// doesn't null the slot would cause element 0 to be disposed twice (by
// `u` and by the next-MoveNext defensive dispose), so the counter would
// overshoot to 3.
//
// ORACLE. The disposals fire after Drive returns, so Main must read
// `R._n` AFTER Drive has finished — `R._n + t` where t = 0 (Drive's
// return value) is the cleanest expression of that ordering. The
// destructor's side-effect on the static counter is observable from
// Main only because the call expression `Drive()` completes before its
// using-vars fire; the using-vars' discharges happen at the §28.3
// owned-local-discharge points (the end of the enclosing scope), which
// is Drive itself, not the call-site.
// ===========================================================================

BEZEL_TEST(owning_enumerator_pending_value_1794,
           manual_protocol_mixed_taken_and_untaken_spec_29_1) {
    const std::string src =
        "module M; " + kCountR +
        "class T "
        "  private static method Drive(): int "
        "    using var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    var drained := a.Drain(); "
        "    using var en := drained.GetOwningEnumerator(); "
        "    if en.MoveNext() then "
        "      using var u := en.TakeCurrent(); "
        "      /* First element disposed by `u`'s using-var scope-end. */ "
        "    end; "
        "    if en.MoveNext() then "
        "      /* Second MoveNext pops element 1 into pending — NO TakeCurrent. "
        "         wrapper.Dispose must reclaim this advanced-but-untaken "
        "         value when `en` scope ends. */ "
        "    end; "
        "    return 0; "
        "  end Drive; "
        "  public static method Main(): int "
        "    var t := Drive(); "
        "    return R._n + t; "
        "  end Main; end T;";

    // Value path: 2 elements disposed exactly once (1 from `u`, 1 from
    // wrapper.Dispose on the untaken pending). R._n at Main = 2, t = 0,
    // so the program returns 2.
    int rcValue = runAotExitCode(src);
    BEZEL_EXPECT_TRUE(rcValue != -999);
    BEZEL_EXPECT_EQ(rcValue, 2);

    // ASan path: no leak (the untaken element would leak if the wrapper
    // didn't dispose it) and no UAF (the taken element would be reused
    // after `u`'s dispose if TakeCurrent didn't null the slot).
    int rcAsan = runUnderASan(src);
    BEZEL_EXPECT_TRUE(rcAsan != -1);
    BEZEL_EXPECT_EQ(rcAsan, 0);
}