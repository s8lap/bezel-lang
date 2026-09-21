// Codegen — a non-escaping `string` LOCAL that holds a +1 value at scope end
// must be released exactly once, INCLUDING when the value arrived by (or was
// mutated through) a `+=` compound concat and INCLUDING a `string` local used
// only as a `+=` borrow operand (§4.6 / §28.9, issue #1486).
//
// Root cause: the string-local escape walk (collectStringReleaseEligible /
// StringEscapeWalker in LlvmBackend_Setup.cpp) over-approximated a `+=`
// (compound-assign) as an ESCAPE in two directions:
//   * the TARGET `s += …` was treated as a reassignment-escape, so a
//     non-escaping accumulator was excluded from the scope-end release and
//     leaked its final +1 (bounded 1-block leak per accumulator); and
//   * a bare-identifier RHS OPERAND `… += t` was treated as an escape of `t`,
//     so a non-escaping producer-held local used as a concat operand leaked.
// But a string `+=` is a CONCAT: it borrow-reads the RHS into a FRESH buffer
// (never retaining it into a slot) and maintains the target slot's independent
// +1 (release-old / store fresh +1). So neither operand becomes a lasting
// alias, and a `+=`-mutated / `+=`-operand string local — when otherwise
// non-escaping — is released exactly once at scope end.
//
// The issue's "inconsistent by producer kind" symptom (`s += tag(i)` looked
// clean under LSan while `s += i.ToString()` leaked, in the identical loop
// shape) was a LeakSanitizer REACHABILITY artifact — under a raw malloc/free
// interposer BOTH leaked the final accumulator; LSan merely found the tag-call
// result still reachable at exit. The underlying gap was uniform.
//
// Two complementary checks (mirroring string_producer_leak_1481_1482_test.cpp):
//   * a JIT run: the program still produces the right exit code (the concat is
//     unaffected), so the added scope-end release did not corrupt the result.
//   * a LeakSanitizer native run (load-bearing for the leak fix): before the
//     fix LSan reports the leaked accumulator / operand; after, it is clean. A
//     borrow-operand-read-after case additionally proves no double-free / UAF
//     (ASan would report it).

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
// string_producer_leak_1481_1482_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1486_lsan_" + std::to_string(counter++);
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
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 == LSan clean (no leaked accumulator, no UAF)
}

// ── (A) §4.6 issue #1486 repro A: producer-init accumulator + `+= i.ToString()`
//        loop — the final accumulator was leaked. "h01234" → length 6. ──────────
const char* kProducerInitToStringLoop_1486 =
    "module M; class C public static method Main(): int "
    "  var s := \"\" + \"h\"; var i := 0; "
    "  while i < 5 do s += i.ToString(); i := i + 1; end; "
    "  return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, producer_init_tostring_loop_released) {
    expectRunsAndLeakFree(kProducerInitToStringLoop_1486, 6);
}

// ── (B) §4.6 issue #1486 repro B: a producer-held local `t` used as a `+=`
//        borrow operand and READ after — `t` was leaked. t="yz" (2), s="ayz"
//        (3); (3 + 2) = 5. The read-after also proves no UAF: the scope-end
//        releases run only after the return value is computed. ─────────────────
const char* kOperandBorrowReadAfter_1486 =
    "module M; class C public static method Main(): int "
    "  var t := \"y\" + \"z\"; var s := \"\" + \"a\"; s += t; "
    "  return (s.Length + t.Length) mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, operand_borrow_read_after_released_no_uaf) {
    expectRunsAndLeakFree(kOperandBorrowReadAfter_1486, 5);
}

// ── (C) §4.6 issue #1486 repro C: literal-init accumulator + `+= "!"` —
//        the final accumulator was leaked. "hello!" → length 6. ───────────────
const char* kLiteralInitConcatAssign_1486 =
    "module M; class C public static method Main(): int "
    "  var s := \"hello\"; s += \"!\"; return s.Length mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, literal_init_concat_assign_released) {
    expectRunsAndLeakFree(kLiteralInitConcatAssign_1486, 6);
}

// ── (D) The issue's "CLEAN" contrast: `+= tag(i)` (user-method producer) in the
//        identical loop shape. Was reachability-masked under LSan; still leaked
//        the accumulator at the raw allocator. Now genuinely released.
//        "sx0x1x2x3x4" → length 11. ────────────────────────────────────────────
const char* kUserMethodProducerLoop_1486 =
    "module M; class C "
    "  static method tag(n: int): string return \"x\" + n.ToString(); end tag; "
    "  public static method Main(): int "
    "    var s := \"\" + \"s\"; var i := 0; "
    "    while i < 5 do s += tag(i); i := i + 1; end; "
    "    return s.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, user_method_producer_loop_released) {
    expectRunsAndLeakFree(kUserMethodProducerLoop_1486, 11);
}

// ── Regression guard: a `+=` accumulator that ESCAPES via `return` must
//        TRANSFER its +1 to the caller — not be released at scope end (which
//        would double-free / strand the returned value). acc() = "a012" (4);
//        Main returns 4. JIT correctness + LSan clean (transfer, no leak/UAF). ─
const char* kReturnedAccumulatorTransfer_1486 =
    "module M; class C "
    "  static method acc(): string "
    "    var s := \"\" + \"a\"; var i := 0; "
    "    while i < 3 do s += i.ToString(); i := i + 1; end; "
    "    return s; "
    "  end acc; "
    "  public static method Main(): int "
    "    var r := acc(); return r.Length mod 256; "
    "  end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, returned_accumulator_transfers_no_double_free) {
    expectRunsAndLeakFree(kReturnedAccumulatorTransfer_1486, 4);
}

// ── Regression guard: a `+=` accumulator reused as a plain `+` operand into a
//        second local — both non-escaping locals released exactly once, no
//        double-free of the shared read. a="ab" (2), c="ab!" (3); (2+3)=5. ─────
const char* kAccumulatorReusedAsOperand_1486 =
    "module M; class C public static method Main(): int "
    "  var a := \"\" + \"a\"; a += \"b\"; var c := a + \"!\"; "
    "  return (a.Length + c.Length) mod 256; "
    "end Main; end C;";
BEZEL_TEST(codegen_string_accumulator_leak_1486, accumulator_reused_as_operand_no_double_free) {
    expectRunsAndLeakFree(kAccumulatorReusedAsOperand_1486, 5);
}

}  // namespace
