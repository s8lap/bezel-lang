// Codegen / stdlib §3.1 (#1185): StringBuilder.Append of a +1 string-PRODUCER
// result must not leak the appended string's backing.
//
// Before the fix the builder treated its parts list as BORROWS: Append stored
// the argument's pointer without retaining it, and the builder's `using`
// disposal never freed the parts array (nor released any part). So a fresh +1
// string-producer result handed to Append leaked:
//
//   * a DIRECT producer (`sb.Append("a" + "b")`, `sb.Append(string.Join(...))`,
//     `sb.Append(i.ToString())`) minted rc=1, was stored as a borrow, and the
//     +1 was never dropped;
//   * a LOCAL bound from a producer (`var s := a + b; sb.Append(s)`) was marked
//     escaping by the conservative escape walk (correctly NOT released at scope
//     end to avoid a dangle) but the Append path took no ownership either, so
//     the +1 leaked;
//   * and the builder's own bz_array_t parts backing leaked on disposal (~0.5
//     KB) regardless of the argument shape.
//
// The fix makes the builder OWN each part (mirroring a borrowing Array<string>
// and lowerStringFieldStore, §4.6/§28.9): Append RETAINS a borrow argument
// (TRANSFERS a +1 producer — no retain), the constructor does the same for its
// initial part, Clear and the Insert/Remove/Replace flatten-resets release the
// old parts, and the builder's `using` disposal now frees the parts array with a
// string-element release. Because Append retains, passing a string LOCAL to it
// is a non-escaping use (the builder's own +1 keeps the value alive for its
// lifetime), so the escape walk releases the caller's +1 at scope end — no leak,
// no UAF.
//
// These LSan checks read `sb.Length` (which sums part lengths without allocating)
// rather than `sb.ToString()`: ToString returns a separate +1 string whose own
// scope-end release is a DISTINCT, pre-existing gap (sb.ToString() is not yet
// recognized as a +1 producer), and using it here would mask the Append-producer
// leak the fix targets. A ToString-based check is included as JIT-only
// correctness (the Append itself is unaffected).
//
// Each LSan case FAILS before the fix (leaked producer + parts array) and is
// clean after. A borrow-operand guard additionally proves no double-free / UAF
// (ASan would surface a premature release of the shared reference).

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

namespace fs = std::filesystem;

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Lower `source` to an object file, link against an ASan-instrumented runtime,
// run under LeakSanitizer. Returns 0 (clean), 1 (sanitizer reported a
// leak/UAF), or -1 (toolchain unavailable → SKIP). Mirrors
// string_producer_leak_1481_1482_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1185_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/UAF
    return 0;                                                      // clean
}

void expectRunsAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;    // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 == LSan clean (no leaked producer, no UAF)
}

} // namespace

// ── #1185: the load-bearing leak — a +1 producer appended in a loop ────────
//
// NOTE on the Length read: `sb.Length` sums the part lengths WITHOUT allocating
// (no ToString), so the ONLY thing that can leak here is the per-Append producer
// + the builder's parts array — exactly #1185. Before the fix each iteration
// leaked its producer (UNBOUNDED — 5 blocks) plus the parts array; after, the
// whole program is leak-free. "x0x1x2x3x4" → Length 10. A `+`-producer, an
// `i.ToString()` producer, a `string.Join` producer, and a user-method producer
// all route through the SAME Append retain/transfer path exercised here.
const char* kDirectProducerLoop_1185 =
    "module M; import System.Collections.Generic; class C "
    "  static method tag(n: int): string return \"x\" + n.ToString(); end tag; "
    "  public static method Main(): int "
    "    var i := 0; using var sb := new StringBuilder(); "
    "    while i < 5 do sb.Append(\"x\" + i.ToString()); i := i + 1; end; "
    "    return sb.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           direct_producer_loop_released) {
    expectRunsAndLeakFree(kDirectProducerLoop_1185, 10);
}

// The issue's exact repro shape: a LOCAL bound from a +1 producer
// (`var s := string.Join(...)`), then `sb.Append(s)`. Before the fix the
// escape walk excluded `s` from scope-end release (avoids a dangle) but Append
// took no ownership, so `s` leaked. The fix's escape-walker relaxation (Append
// retains ⇒ the local is non-escaping) releases `s` at scope end after the
// builder is disposed. Five iterations of "ab,cd" → Length 25.
const char* kLocalProducerLoop_1185 =
    "module M; import System.Collections.Generic; class C "
    "  public static method Main(): int "
    "    using var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
    "    var i := 0; using var sb := new StringBuilder(); "
    "    while i < 5 do var s := string.Join(\",\", xs); sb.Append(s); "
    "                                i := i + 1; end; "
    "    return sb.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           local_producer_loop_released) {
    expectRunsAndLeakFree(kLocalProducerLoop_1185, 25);
}

// `new StringBuilder(initial: string)` with a +1 producer initial part — the
// ctor shares Append's retain/transfer path. Before the fix the producer
// initial leaked. "hello"+"!" = "hello!", Length 6.
const char* kCtorProducer_1185 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(\"hello\" + \"!\"); "
    "  return sb.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           ctor_producer_released) {
    expectRunsAndLeakFree(kCtorProducer_1185, 6);
}

// A producer appended ONCE (not in a loop) — the bounded, single-occurrence
// shape. Before the fix the producer + the parts array leaked; after, clean.
const char* kSingleProducerAppend_1185 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\" + \"cd\"); "
    "  return sb.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           single_producer_append_released) {
    expectRunsAndLeakFree(kSingleProducerAppend_1185, 4);
}

// JIT-only correctness: `sb.ToString()` of appended producers must still
// produce the right string (the retain/transfer must not corrupt the parts).
// JIT-only because ToString returns its own +1 string whose scope-end release
// is the separate, pre-existing gap noted above. "ab"+"cd" then "ef"+"gh"
// → "abcdefgh", Length 8.
const char* kToStringCorrectness_1185 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\" + \"cd\"); "
    "  sb.Append(\"ef\" + \"gh\"); return sb.ToString().Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           tostring_of_appended_producers_correct_value) {
    BEZEL_EXPECT_EQ(run(kToStringCorrectness_1185), 8);
}

// Regression guard: a string-LOCAL borrow appended and then READ must NOT be
// prematurely freed (no use-after-free). `s` is read AFTER the Append; a wrong
// release would be a UAF — the in-process JIT run (the load-bearing correctness
// check) surfaces it as a crash/wrong result. s="yz"; sb holds "yz"; result
// (2 + 2) = 4.
const char* kLocalBorrowAppendNoUaf_1185 =
    "module M; class C public static method Main(): int "
    "  var s := \"y\" + \"z\"; using var sb := new StringBuilder(); sb.Append(s); "
    "  return (s.Length + sb.Length) mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_append_producer_leak_1185,
           local_borrow_append_no_uaf) {
    BEZEL_EXPECT_EQ(run(kLocalBorrowAppendNoUaf_1185), 4);
}
