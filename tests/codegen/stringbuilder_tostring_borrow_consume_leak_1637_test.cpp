// Codegen / §28.3 (producers) / §28.9 (runtime-managed types), spec v0.188
// (#1637): the +1 string minted by StringBuilder.ToString() must be released
// when it is consumed by a BORROWING read with no intervening owning local —
// the canonical shape `sb.ToString().Length`.
//
// `sb.ToString()` lowers (tryLowerCollectionInstanceCall, LlvmBackend_Calls.cpp)
// to a fresh heap `bz.string`: bz_strbuilder_to_string malloc's the concatenated
// buffer and initStringRc sets rc := 1. That is a +1 single-reference producer,
// exactly like `+`/interpolation/range-slice/primitive-ToString/string.Join —
// its single reference must be released once when consumed. `string` is
// runtime-managed (§28.9: "its memory is reclaimed automatically"; the producers
// table row in §28.3 marks a `string`-returning call runtime-managed), and the
// implementation reclaims it by reference counting.
//
// Before the fix `isPlusOneStringProducer` (LlvmBackend_Setup.cpp) recognized
// `+`, interpolation, range-slice, string.Join, primitive ToString(), user-method
// `string` returns, and `() -> string` closure calls — but NOT
// StringBuilder.ToString() (a builtin with no resolved MethodSymbol). So
// `sb.ToString().Length` read the length and never dropped the ToString result's
// single reference: the 32-byte string header + its data buffer leaked (~37
// bytes). The fix recognizes a ToString() call on a StringBuilder-typed receiver
// as a +1 producer, so the borrowing read releases it after the read and a
// `var s := sb.ToString()` local transfers the +1 into its slot (released at
// scope end when non-escaping).
//
// Each LSan case FAILS before the fix (the leaked ToString result → LSan exit 99
// → lsan==1) and is clean after (lsan==0). ASan additionally proves the release
// is not premature — no double-free / use-after-free of the shared reference.
// Same AOT+LSan legged harness as stringbuilder_append_producer_leak_1185_test
// and string_producer_leak_1481_1482_test.

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
#include "support/unique_stem.hpp"

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
// stringbuilder_append_producer_leak_1185_test.cpp.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_1637_lsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();

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
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/UAF
    return 0;                                                      // clean
}

void expectRunsAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;    // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 == LSan clean (no leaked ToString result, no UAF)
}

} // namespace

// ── #1637: the load-bearing leak — sb.ToString() consumed by a borrowing read ──
//
// The issue's exact repro: `sb.ToString().Length`. The +1 ToString result is
// read for its Length and, before the fix, never released — the 32-byte header +
// 5-byte data buffer ("abcd\0") leaked. After the fix the borrowing read of
// `.Length` releases the producer. "abcd".Length = 4.
const char* kToStringDotLength_1637 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "  return sb.ToString().Length; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_dot_length_released) {
    expectRunsAndLeakFree(kToStringDotLength_1637, 4);
}

// A LOCAL bound from the ToString producer: `var s := sb.ToString(); s.Length`.
// The +1 transfers into `s`'s slot (no borrow-retain) and the scope-end release
// drops it once. Before the fix `s` was treated as a borrow (retained → rc 2),
// and the scope-end release left rc 1 — the producer's +1 leaked. "abcd" → 4.
const char* kToStringLocalTransfer_1637 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "  var s := sb.ToString(); return s.Length; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_local_transfer_released) {
    expectRunsAndLeakFree(kToStringLocalTransfer_1637, 4);
}

// The ToString producer as a `+` OPERAND: `("x" + sb.ToString()).Length`. The
// concat consumes (and releases) the ToString result via releaseConcatIntermediate
// now that it is a recognized producer, and the outer concat result is itself a
// +1 producer released by the `.Length` read. "xabcd" → 5.
const char* kToStringConcatOperand_1637 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "  return (\"x\" + sb.ToString()).Length; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_concat_operand_released) {
    expectRunsAndLeakFree(kToStringConcatOperand_1637, 5);
}

// Two independent ToString() producers in one expression — each mints and each
// must be released exactly once (no leak, no double-free of a shared reference).
// (4 + 4) = 8.
const char* kToStringTwice_1637 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "  return sb.ToString().Length + sb.ToString().Length; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_twice_each_released) {
    expectRunsAndLeakFree(kToStringTwice_1637, 8);
}

// Field-store TRANSFER (§4.6/§28.9): storing the ToString producer into a class
// string field transfers the +1 (lowerStringFieldStore now sees a producer and
// does NOT double-retain); the field's reference is released on the owner's
// disposal. No leak, no double-free. "abcd" → 4.
const char* kToStringFieldStore_1637 =
    "module M; class C "
    "  public Name: string := \"\"; "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "    using var o := new C(); o.Name := sb.ToString(); "
    "    return o.Name.Length; "
    "  end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_field_store_transferred) {
    expectRunsAndLeakFree(kToStringFieldStore_1637, 4);
}

// Control: `sb.Length` (no ToString, no allocation) — leak-free before AND after
// the fix. Pins that the fix does not perturb the non-producer path. Length 4.
const char* kSbLengthControl_1637 =
    "module M; class C public static method Main(): int "
    "  using var sb := new StringBuilder(); sb.Append(\"ab\"); sb.Append(\"cd\"); "
    "  return sb.Length; "
    "end Main; end C;";
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           sb_length_control_leak_free) {
    expectRunsAndLeakFree(kSbLengthControl_1637, 4);
}

// JIT-only correctness: the ToString result's characters must be intact when
// consumed (the release must fire AFTER the read, not before → no UAF that would
// corrupt the value). "abcd".Length = 4.
BEZEL_TEST(codegen_stringbuilder_tostring_borrow_consume_leak_1637,
           tostring_value_intact) {
    BEZEL_EXPECT_EQ(run(kToStringDotLength_1637), 4);
}
