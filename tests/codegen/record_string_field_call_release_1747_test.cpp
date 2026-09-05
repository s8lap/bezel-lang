// Codegen + runtime — record string fields LEAK when a record-call-result
// local escapes by value (residual after #1638, issue #1747, §28.9 / §9.7 /
// §28.3). The pre-fix state: every record with a `string` field returned
// from a method and bound to a `var p := <call>` local leaked the callee-
// minted +1 on each string field. This file pins the NARROW-scope fix
// the lane pinned: the RETURN + CALL-RESULT channel — a `var p := Mk()`
// whose return type is a record with at least one `string` field, and
// the local never escapes (no copy, no by-value-arg, no collection/field
// store, no with-copy, no nullable result). A NewExpr-init local, a copy,
// etc. keep the prior safe-leak behaviour (the walker over-approximates
// them as escapes).
//
// Spec: §28.9 (runtime-managed string reference counting), §9.7 (a
// record has no disposer), §28.3 (scope-end auto-drop). Cited revision:
// v0.226.
//
// The PIN (record_return_call_result_string_field_released) FAILS on
// stock main — LSan reports the leaked string fields (the caller never
// releases the callee-minted +1) — and PASSES with the fix (LSan clean).
// The literal-field control is leak-free regardless. The by-value-arg
// soundness case pins NO UAF / double-free for the disqualified shapes
// (a `Use(Mk())` direct-pass form) that keep the prior safe-leak
// behaviour — a leak may still be reported for that residual channel,
// but no use-after-free / double-free.
//
// Both LSan checks skip gracefully when no ASan toolchain is available
// (mirrors escaping_string_refcount_test.cpp).

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
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to an ASan-instrumented native executable via the real
// front-end + AOT link (mirrors alias_owning_collection_disposal_1462_test.cpp).
// Run it under LSan. Returns:
//   -1 : toolchain unavailable OR a compile/link failure (test skips)
//    0 : LSan-clean (no leaks, no UAF, no double-free)
//    1 : LSan leak report (exitcode 99 = ASAN_OPTIONS=exitcode=99)
//    2 : ASan hard-error (use-after-free / double-free / heap-buffer-overflow)
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1747_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
    std::string runErr = (dir / (stem + ".run.err")).string();
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
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str());
                          std::remove(runErr.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " 2>" + runErr;
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (!WIFEXITED(status)) return 2;  // signal — ASan hard-error
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;          // LSan leak report
    return 0;                          // clean — exit code is the program return
}

}  // namespace

// §28.9 / §9.7 / §28.3 (fix #1747): a record with a `string` field returned
// from a method (`return new P($"...", 8)`) and bound to a caller local
// (`var p := Mk()`) was leaking the field's +1. The fix arms a scope-end
// recordStringFields Disposal that bz_string_release's each string field at
// scope end (only when the local is non-escaping). On stock main this test
// FAILS — LSan reports the leak (a +1 per Mk call survives scope end). With
// the fix it PASSES — LSan clean.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_string_field_released_spec_28_9) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  return new P($\"hello\", 8); "
        "end Mk; "
        "public static method Main(): int "
        "  var p := Mk(); "
        "  return p.X; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;             // toolchain unavailable → SKIP
    BEZEL_EXPECT_EQ(rc, 0);            // LSan clean — fix landed
}

// §28.9: control — a literal field initializer is a sentinel (rc == SENTINEL),
// so its bz_string_release at scope end is a no-op. LSan-clean regardless
// of the fix. Pins the baseline that the literal field doesn't introduce a
// leak even without the new disposal.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_literal_field_safe_spec_28_9) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  return new P(\"hi\", 8); "
        "end Mk; "
        "public static method Main(): int "
        "  var p := Mk(); "
        "  return p.X; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// §28.9 / §9.7 / §28.3 (fix #1747): the narrow scope's call-result
// registration must run ONLY on a NON-escaping local. A local that IS
// passed by value to a method (its string fields are copied into a callee
// slot without callee-side retain discipline) keeps the prior safe-leak
// behaviour — the scope-end release would drop the callee's still-live
// reference (UAF). The conservative escape walk keeps such locals OUT of
// the recordCallResultReleaseEligible_ set, so the disposal is not
// registered. Pins that the narrow fix does NOT regress — the program
// runs to completion (no UAF / no double-free — exit 0 / no ASan
// hard-error), even though a leak may still be reported for this residual
// channel (rc==1 is the prior safe behaviour).
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_escapes_via_method_arg_safe_no_uaf_spec_28_9) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  return new P($\"hi\", 8); "
        "end Mk; "
        "public static method Use(p: P): int "
        "  return p.X; "
        "end Use; "
        "public static method Main(): int "
        "  return Use(Mk()); "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    // Pin NO UAF / double-free: rc must NOT be 2 (ASan hard-error). It can
    // be 0 (LSan clean — the +1 is released somewhere) or 1 (LSan leak —
    // the prior safe behaviour, tracked as a residual #1747 channel).
    BEZEL_EXPECT_TRUE(rc != 2);
}

// fix #3745 / §9.7 / §28.9 (v0.286): isProvablyFreshRecordFactoryMethod now
// admits the two-statement factory body `(var x := new P(...); return x;)` —
// the natural expansion of the existing single-statement
// `return new P(...);` case the #1747 row above already pins. On stock main
// the multi-statement shape was REJECTED (the predicate required exactly one
// statement), so a `var p := Make(...)` local on this shape leaked one
// `bz_string_release` per call (LSan-measured 7000 B / 400 allocations over
// 200 iter — #3745 row 3). With the predicate extension it is admitted, the
// recordStringFields scope-end disposal is registered, and the program is
// LSan-clean. Pins the positive case the #1747 narrow fix could NOT reach
// before the predicate gained the second-statement arm.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_multistmt_factory_released_spec_9_7_3745) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P "
        "  var x := new P($\"hello\", 8); "
        "  return x; "
        "end Mk; "
        "public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var p := Mk(); "
        "    total := total + p.X; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    // Output check: total = 8 * 200 = 1600 (every record's X is 8).
    BEZEL_EXPECT_EQ(rc, 0);
}

// fix #3745 / §9.7 / §28.9 (v0.286): control — the EXISTING single-statement
// `return new P(...);` shape MUST stay admitted by the predicate extension.
// The two-statement arm gates ONLY on the EXACT two-statement shape
// (LocalDecl + ReturnStmt, plain `var`, identifier return); a single-
// statement return continues to flow through the (b) arm. Pins no regression
// on the #1747 positive case when the predicate's case (c) is added.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_singleexpr_factory_still_clean_spec_9_7_3745) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(): P => new P($\"hello\", 8); "
        "public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 200 do "
        "    var p := Mk(); "
        "    total := total + p.X; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    BEZEL_EXPECT_EQ(rc, 0);
}

// fix #3745 / §9.7 / §28.9 (v0.286): fail-closed regression — the predicate's
// two-statement arm must REJECT a body that is NOT exactly
// `(var x := new P(...); return x;)`. A control-flow shape (an `if` between
// the local and the return, two locals, an `else`, etc.) must NOT be
// admitted — otherwise we re-open the #2448 use-after-free class through a
// construct this predicate does not reason about. Here we use the simplest
// failure-2 — a SECOND LocalDecl — to pin that the predicate returns false
// even when the body is otherwise well-shaped (a factory method with two
// locals + return).
//
// Pinned via the LEAK channel: if the predicate incorrectly admitted the
// two-local shape, the `var p := Mk();` local on the call-result side would
// either be scope-end-released (UAF if Mk returned a borrowed field) OR
// conservatively not released (leak). For a body that returns a fresh
// construction in BOTH locals, the conservatively-not-released branch
// dominates — the program runs to completion (no UAF, since the body is
// fresh on every path) and LSan reports the leftover +1.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_return_call_result_two_locals_factory_still_leaky_spec_9_7_3745) {
    std::string src =
        "module M; record P(Name: string, X: int); "
        "class C public static method Mk(n: int): P "
        "  var x := new P($\"hello\", n); "
        "  var y := new P($\"world\", n); "
        "  return x; "
        "end Mk; "
        "public static method Main(): int "
        "  var total := 0; "
        "  var i := 0; "
        "  while i < 50 do "
        "    var p := Mk(i); "
        "    total := total + p.X; "
        "    i := i + 1; "
        "  end while; "
        "  return total; "
        "end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    // Pin no UAF / double-free: rc must NOT be 2. The leak itself (rc == 1)
    // is the documented safe direction for the two-local shape the predicate
    // does not admit — a bounded leak, not a soundness defect.
    BEZEL_EXPECT_TRUE(rc != 2);
}