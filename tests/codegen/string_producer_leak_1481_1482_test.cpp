// Codegen — a string-typed operand that is itself a +1 PRODUCER (a call
// returning `string`, `i.ToString()`, a nested `+`/interpolation, `string.Join`,
// a `() -> string` closure call) must be released after a concat borrows it, in
// BOTH of these string-concat lowering paths:
//
//   * `s += <producer>`  (compound concat, §4.6)  — issue #1481
//   * `$"{ <producer> }"` interpolation hole (§18) — issue #1482
//
// The two sites recognized only a FRESHLY-RENDERED intermediate (a non-string
// RHS converted via emitPrimitiveToString → `rStr != rhs`, or a numeric hole
// → `pv != srcEv`) and released that; a STRING-typed operand was assumed to be a
// borrow (`rStr == rhs` / `pv == srcEv`) and left alone. But a string-typed
// operand that is a +1 producer also hands the caller its single reference — the
// concat borrows (copies) it and nothing else reclaims it, so it leaked,
// unboundedly in a loop. The fix releases the operand when
// isPlusOneStringProducer(expr) is true, mirroring the string `+` operator's
// operand discipline (LlvmBackend_Exprs.cpp:6055). A string-typed BORROW operand
// (a named local/field per §28.9) is NOT a producer and must NOT be released (no
// double-free / use-after-free of the shared reference).
//
// Two complementary checks (mirroring console_write_leak_test.cpp):
//   * a JIT run: the program still produces the right exit code (the concat
//     itself is unaffected), so the added release did not corrupt the result.
//   * a LeakSanitizer native run (load-bearing for the leak fix): before the fix
//     LSan reports the leaked producer(s); after, it is clean. A borrow-operand
//     case additionally proves no double-free (ASan would report a UAF).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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
// console_write_leak_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1481_lsan_" + std::to_string(counter++);
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -1;
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

// ── #1481: `s += <string producer>` ────────────────────────────────────────
//
// NOTE on accumulator provenance: these repros initialize `s` from a +1
// PRODUCER (`"" + "…"`), not a bare string literal. A producer-initialized
// string local is released at scope end (even across a loop), so the ONLY
// thing that can leak here is the per-`+=` RHS producer — exactly #1481. A
// LITERAL-initialized accumulator (`var s := "…"`) additionally leaks its final
// +1 value at scope end regardless of #1481; that is a SEPARATE, pre-existing
// gap tracked in its own issue (see the run's discovery log), deliberately
// factored out so these tests isolate the #1481 RHS-producer release.

// The load-bearing #1481 leak test: `s += <user-method string producer>` in a
// loop, with a producer-init accumulator. tag(0..4) each mint a fresh +1
// string. Before the fix each leaked (UNBOUNDED — 5 blocks); after, the whole
// program is leak-free. Result "sx0x1x2x3x4", length 11. `i.ToString()` and
// every other +1 producer RHS route through the SAME isPlusOneStringProducer
// release path exercised here (their extra scope-end coverage is JIT-only below
// to avoid the separate accumulator-release gap).
const char* kCompoundUserMethodLoop_1481 =
    "module M; class C "
    "  static method tag(n: int): string return \"x\" + n.ToString(); end tag; "
    "  public static method Main(): int "
    "    var s := \"\" + \"s\"; var i := 0; "
    "    while i < 5 do s += tag(i); i := i + 1; end; "
    "    return s.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1481, compound_user_method_producer_loop_released) {
    expectRunsAndLeakFree(kCompoundUserMethodLoop_1481, 11);
}

// JIT-only correctness: `s += i.ToString()` in a loop must still produce the
// right string (the added release must not corrupt the concat). "h01234" → 6.
const char* kCompoundToStringLoop_1481 =
    "module M; class C public static method Main(): int "
    "  var s := \"\" + \"h\"; var i := 0; "
    "  while i < 5 do s += i.ToString(); i := i + 1; end; "
    "  return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1481, compound_tostring_loop_correct_value) {
    BEZEL_EXPECT_EQ(run(kCompoundToStringLoop_1481), 6);
}

// JIT-only regression guard: `s += <string local borrow>` must NOT release the
// borrow. `t` is a heap +1 local READ AFTER the `+=`; a wrong release would be
// a use-after-free — the in-process JIT run (the load-bearing correctness
// check) surfaces such a double-free as a crash/wrong result. t="yz"; s="ayz";
// result (3 + 2) = 5.
const char* kCompoundStringLocalBorrow_1481 =
    "module M; class C public static method Main(): int "
    "  var t := \"y\" + \"z\"; var s := \"\" + \"a\"; s += t; "
    "  return (s.Length + t.Length) mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1481, compound_string_local_borrow_no_uaf) {
    BEZEL_EXPECT_EQ(run(kCompoundStringLocalBorrow_1481), 5);
}

// ── #1482: `$"{ <string producer> }"` interpolation hole ────────────────────

// A user-method string producer used as an interpolation hole. tag(1)="x1";
// result "x1 done", length 7. Before the fix the producer leaked.
const char* kInterpUserMethodHole_1482 =
    "module M; class C "
    "  static method tag(n: int): string return \"x\" + n.ToString(); end tag; "
    "  public static method Main(): int "
    "    var s := $\"{tag(1)} done\"; return s.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1482, interp_user_method_hole_released) {
    expectRunsAndLeakFree(kInterpUserMethodHole_1482, 7);
}

// An explicit `.ToString()` producer as a hole (distinct from implicit numeric
// formatting). n.ToString()="42"; result "42!", length 3.
const char* kInterpToStringHole_1482 =
    "module M; class C public static method Main(): int "
    "  var n := 42; var s := $\"{n.ToString()}!\"; return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1482, interp_tostring_hole_released) {
    expectRunsAndLeakFree(kInterpToStringHole_1482, 3);
}

// A nested concat producer as a hole. ("a"+"b")="ab"; result "ab.", length 3.
const char* kInterpConcatHole_1482 =
    "module M; class C public static method Main(): int "
    "  var s := $\"{\"a\" + \"b\"}.\"; return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1482, interp_concat_hole_released) {
    expectRunsAndLeakFree(kInterpConcatHole_1482, 3);
}

// Regression guard: a string-LOCAL borrow hole must NOT be released (no
// double-free / UAF). result "hi!", length 3.
const char* kInterpStringLocalBorrowHole_1482 =
    "module M; class C public static method Main(): int "
    "  var g := \"hi\"; var s := $\"{g}!\"; return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1482, interp_string_local_borrow_hole_not_released) {
    expectRunsAndLeakFree(kInterpStringLocalBorrowHole_1482, 3);
}

// Loop: a producer hole rendered each iteration must not leak per-iteration.
const char* kInterpProducerHoleLoop_1482 =
    "module M; class C "
    "  static method tag(n: int): string return \"x\" + n.ToString(); end tag; "
    "  public static method Main(): int "
    "    var i := 0; var acc := 0; "
    "    while i < 5 do var s := $\"{tag(i)} y\"; acc := acc + s.Length; i := i + 1; end; "
    "    return acc mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_string_producer_leak_1482, interp_producer_hole_loop_no_unbounded_leak) {
    // tag(0..4) = "x0".."x4" (len 2 each); s = "xN y" (len 4 each); acc = 20.
    expectRunsAndLeakFree(kInterpProducerHoleLoop_1482, 20);
}

}  // namespace
