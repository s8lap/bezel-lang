// Codegen + sema — #466: chaining a member call onto the VOID result of an
// in-place collection method (e.g. `a.Reverse().ToArray()` on a concrete
// Array<T>) must NOT corrupt the heap.
//
// ROOT CAUSE (#466). Per §26.5 ("instance members win") the call `a.Reverse()`
// on a concrete `Array<int>` resolves to the IN-PLACE void `Array<T>.Reverse()`
// (stdlib §3), which SHADOWS the LINQ extension `Reverse(): IEnumerable<T>`
// (stdlib §5). `a.Reverse().ToArray()` therefore calls `.ToArray()` on `unit`.
// Sema set no type for the inner void call, and none of the LINQ/collection
// lowerings matched the outer `.ToArray()`, so codegen produced NO value for
// the chained call. The `using var rev: Array<int>` slot was then left
// uninitialised, read (as `rev.Length`), and freed at scope end via
// `bz_array_free` — `free(): invalid pointer` (exit 134). The chained form
// `a.Where(..).Reverse().ToArray()` was unaffected (fixed in #449) because its
// inner `Reverse()` receiver is a lazy IEnumerable<T>, which binds the §5 lazy
// `Reverse` (an IEnumerable<T>-typed value), not the in-place instance method.
//
// FIX (spec-faithful). Since §26.5 makes the in-place instance method win, the
// chained-on-void form is a genuine TYPE ERROR, not a value-producing
// expression. Sema now rejects it with a clear diagnostic instead of letting
// codegen emit a slot-less initializer that crashes at dispose. The legitimate
// in-place use (`a.Reverse();` as a statement) and the lazy LINQ Reverse
// (`a.Where(..).Reverse().ToArray()`) are unchanged.
//
// The crash was heap corruption, so the running cases (in-place mutate + the
// lazy pipeline) carry an ASan/LeakSanitizer leg — the real gate. The
// compile-error cases never produce an executable, so they are verified by
// asserting the diagnostic fires (and the compiler itself does not crash).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir466() {
    return wtScratchDir("wt-466");
}

// Run the JIT pipeline; return the exit code, or -999 if the program did not
// run (compile error / JIT failure).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// True iff the front end rejected `src` during sema (a diagnostic was reported
// and the program did not run).
bool rejectedAtCompile(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

// True iff at least one reported diagnostic message contains `needle`.
bool diagContains(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// ---- ASan leg (mirrors linq_refcount_test.cpp) --------------------------
// Lower `source` to an object via the real native pipeline; -1 on any failure.
int lowerToObject(const std::string& source, const std::string& objPath) {
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
    return 0;
}

// Compile `source` under ASan + LeakSanitizer, run it, and check the exit code.
// Returns 0 (clean: correct value, no leak, no UAF/double-free), 1 (sanitizer
// reported a problem OR wrong value), or -1 (toolchain unavailable: SKIP).
int runUnderAsan(const std::string& source, int expectedExitCode) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable: skip
    auto dir = wtDir466();
    std::string stem = unique_stem("bezel_466rev");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    if (lowerToObject(source, objPath) != 0) return -2;
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    // detect_leaks=1 catches leaks; exitcode=99 is ASan's distinct error code
    // (a UAF/double-free can never be confused with the program's own return).
    std::string runCmd = "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe +
                         " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (!WIFEXITED(status)) return 1;            // crashed/aborted (the bug!)
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;                     // sanitizer reported a problem
    return (code == expectedExitCode) ? 0 : 1;    // wrong value → failure
}

// Assert: program is correct under the JIT AND ASan-clean (no leak / no
// corruption). Skips the ASan leg when no clang++/runtime is available.
void expectValueAndAsanClean(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int a = runUnderAsan(source, expectedExitCode);
    if (a == -1) return;          // toolchain unavailable: skip ASan leg
    BEZEL_EXPECT_EQ(a, 0);        // 0 == correct + ASan clean
}

}  // namespace

// ===========================================================================
// #466 — the crash: chaining `.ToArray()` onto the void in-place `Reverse()`.
// Per §26.5 the in-place instance `Array<T>.Reverse()` (§3) shadows the LINQ
// `Reverse(): IEnumerable<T>` (§5); chaining a member onto its `unit` result is
// a compile error (NOT a runtime crash, and NOT a silently-different lazy form).
// ===========================================================================

// The exact reported repro: `a.Reverse().ToArray()` on a raw Array<int>. Must
// be rejected at compile time, not crash with free()-invalid-pointer (#466).
BEZEL_TEST(array_reverse_chain_466, direct_reverse_toarray_is_rejected_spec_26_5) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); "
        "  using var rev: Array<int> := a.Reverse().ToArray(); "
        "  return rev.Length; "
        "end Main; end C;"));
}

// The diagnostic names the in-place semantics and the §26.5 shadowing rule so
// the programmer can find the lazy-pipeline workaround.
BEZEL_TEST(array_reverse_chain_466, direct_reverse_toarray_diagnostic_cites_26_5) {
    const char* src =
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); "
        "  using var rev: Array<int> := a.Reverse().ToArray(); "
        "  return rev.Length; "
        "end Main; end C;";
    BEZEL_EXPECT_TRUE(diagContains(src, "26.5"));
    BEZEL_EXPECT_TRUE(diagContains(src, "in place"));
}

// Direct Reverse().ToArray() on a SINGLE-element array — still a compile error
// (same in-place-then-chain misuse), no crash.
BEZEL_TEST(array_reverse_chain_466, direct_reverse_toarray_single_rejected_spec_26_5) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(42); "
        "  using var rev: Array<int> := a.Reverse().ToArray(); "
        "  return rev.Length; "
        "end Main; end C;"));
}

// Direct Reverse().ToArray() on an EMPTY array — still a compile error, no crash.
BEZEL_TEST(array_reverse_chain_466, direct_reverse_toarray_empty_rejected_spec_26_5) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  using var rev: Array<int> := a.Reverse().ToArray(); "
        "  return rev.Length; "
        "end Main; end C;"));
}

// Direct Reverse().ToArray() on an Array<string> — element copy would have been
// the runtime concern; here it is rejected at compile time (no crash).
BEZEL_TEST(array_reverse_chain_466, direct_reverse_toarray_string_rejected_spec_26_5) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<string>(); "
        "  a.Add(\"x\"); a.Add(\"y\"); "
        "  using var rev: Array<string> := a.Reverse().ToArray(); "
        "  return rev.Length; "
        "end Main; end C;"));
}

// Direct chaining onto the void in-place Sort() is rejected the same way (#466
// is a general "chain-onto-void-collection-method" bug; Sort hit it too).
BEZEL_TEST(array_reverse_chain_466, direct_sort_toarray_is_rejected_spec_3) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(3); a.Add(1); "
        "  using var s: Array<int> := a.Sort(lambda(x: int, y: int) => x - y).ToArray(); "
        "  return s.Length; "
        "end Main; end C;"));
}

// ===========================================================================
// The legitimate in-place form (§3) is UNAFFECTED and runs cleanly. The source
// array stays usable + correct after the in-place reverse. ASan-gated — proves
// no double-free of the array's single backing buffer.
// ===========================================================================

// §3: in-place Reverse mutates `a`; `a` is still usable afterwards (Length,
// Get(i)) and disposes exactly once. a=[1,2,3] -> reversed [3,2,1]. The full
// ordering is encoded in a value that uniquely identifies [3,2,1] yet stays
// within the 0..255 process exit-code range (WEXITSTATUS truncates to a byte):
// Get(0)*49 + Get(1)*7 + Get(2) = 3*49 + 2*7 + 1 = 162.
BEZEL_TEST(array_reverse_chain_466, inplace_reverse_source_still_usable_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); "
        "  a.Reverse(); "  // [1,2,3] -> [3,2,1]
        "  return a.Get(0) * 49 + a.Get(1) * 7 + a.Get(2); "  // 162
        "end Main; end C;", 162);
}

// §3: in-place Reverse on a single-element array is a no-op; Length is still 1.
BEZEL_TEST(array_reverse_chain_466, inplace_reverse_single_element_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(7); "
        "  a.Reverse(); "
        "  return a.Length * 10 + a.Get(0); "  // 1*10 + 7 = 17
        "end Main; end C;", 17);
}

// §3: in-place Reverse on an empty array is a no-op; Length stays 0.
BEZEL_TEST(array_reverse_chain_466, inplace_reverse_empty_spec_3) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Reverse(); "
        "  return a.Length; "  // 0
        "end Main; end C;", 0);
}

// ===========================================================================
// Regression: the LINQ lazy Reverse (driven from a lazy IEnumerable<T>) still
// works and is ASan-clean. This is the path #449 fixed; #466's sema gate must
// not disturb it — its inner Reverse receiver is lazy (astNode-less sentinel),
// so the in-place shadowing rule does not apply.
// ===========================================================================

// §5: a.Where(odd).Reverse().ToArray() — Where([1,2,3,4,5], odd) = [1,3,5];
// Reverse = [5,3,1]. The full ordering is encoded as rev.Get(0)*49 +
// rev.Get(1)*7 + rev.Get(2) = 5*49 + 3*7 + 1 = 267 wraps mod 256; use a smaller
// base so the value stays <=255: rev.Get(0)*36 + rev.Get(1)*6 + rev.Get(2) =
// 5*36 + 3*6 + 1 = 199 (uniquely identifies [5,3,1]). The source `a` and the
// result `rev` each dispose exactly once (no double-free, no leak).
BEZEL_TEST(array_reverse_chain_466, chained_where_reverse_toarray_still_works_spec_5) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(1); a.Add(2); a.Add(3); a.Add(4); a.Add(5); "
        "  using var rev: Array<int> := "
        "    a.Where(lambda(x) => x mod 2 = 1).Reverse().ToArray(); "
        "  return rev.Get(0) * 36 + rev.Get(1) * 6 + rev.Get(2); "  // 199
        "end Main; end C;", 199);
}

// §5: single-element source through the lazy Reverse. Where(x>0) on [9] = [9];
// Reverse = [9]; rev.Length*10 + rev.Get(0) = 1*10 + 9 = 19.
BEZEL_TEST(array_reverse_chain_466, chained_where_reverse_single_spec_5) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); a.Add(9); "
        "  using var rev: Array<int> := "
        "    a.Where(lambda(x) => x > 0).Reverse().ToArray(); "
        "  return rev.Length * 10 + rev.Get(0); "
        "end Main; end C;", 19);
}

// §5: the source array is still usable + correct AFTER the lazy pipeline ran
// (the lazy Reverse BORROWS the source; it must not consume or corrupt it).
// After Where(..).Reverse().ToArray(), read a.Get(i): a=[4,5,6] is unchanged.
// Encode [4,5,6] uniquely within 0..255: Get(0)*36 + Get(1)*6 + Get(2) =
// 4*36 + 5*6 + 6 = 180.
BEZEL_TEST(array_reverse_chain_466, source_array_intact_after_lazy_reverse_spec_5) {
    expectValueAndAsanClean(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(4); a.Add(5); a.Add(6); "
        "  using var rev: Array<int> := "
        "    a.Where(lambda(x) => x > 0).Reverse().ToArray(); "
        "  return a.Get(0) * 36 + a.Get(1) * 6 + a.Get(2); "  // 180
        "end Main; end C;", 180);
}
