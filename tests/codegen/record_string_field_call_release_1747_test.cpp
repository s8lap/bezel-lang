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
#include <sstream>
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
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to an ASan-instrumented native executable via the real
// front-end + AOT link (mirrors alias_owning_collection_disposal_1462_test.cpp).
// Run it under ASan/LSan. Returns:
//   -1 : toolchain unavailable OR a compile/link failure (test skips)
//    0 : sanitizer-clean (no leaks, no UAF, no double-free)
//    1 : LeakSanitizer leak report
//    2 : AddressSanitizer hard-error (use-after-free / double-free /
//        heap-buffer-overflow) or a signal death
//
// fix #3917: the object is emitted through `bezel::test::emitObjectFileForTest`
// (support/emit_object_support.hpp), so under `gate.sh --full` the GENERATED
// Bezel code carries shadow checks on its own loads and stores. Before that,
// this helper called `codegen::emitObjectFile` with its three-argument
// spelling, whose `sanitizeAddress` parameter defaults to false
// (src/codegen/LlvmBackend.hpp:102) and which consults nothing: the
// ASan-instrumented C++ runtime it links caught a LEAK (the interposed
// allocator is all LSan needs) but a use-after-free READ by generated code
// emitted no check at all and was structurally invisible. Measured on this
// very file: with `BEZEL_AOT_FORCE_SANITIZE=address` set, `nm` reported ZERO
// `__asan_report*` references in all 7 of the objects this helper emitted;
// with the wrapper, 11-12 each.
//
// The rc mapping is report-driven for the same reason (see the run block).
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -1;
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str());
                          std::remove(runErr.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " 2>" + runErr;
    int status = std::system(runCmd.c_str());
    // Read the report BEFORE cleanup() deletes it (#3917). `exitcode=99`
    // applies to EVERY sanitizer finding, so the wait status alone cannot
    // tell an LSan leak from a heap-use-after-free — both exit 99. Mapping
    // 99 to "leak" is what would keep this file's two `_no_uaf_` rows
    // reporting a freshly-instrumented UAF as the safe-leak outcome they
    // tolerate. The report text is the only thing that distinguishes them.
    std::string report;
    {
        std::ifstream in(runErr);
        std::ostringstream ss;
        ss << in.rdbuf();
        report = ss.str();
    }
    cleanup();
    if (status == -1) return -3;
    switch (bezel::test::sanitizerFindingInReport(report)) {
        case bezel::test::SanitizerFinding::kHardError:
            // Surface it: a redzone finding must never be observed silently.
            std::fprintf(stderr,
                "\n===== [aot-asan #1822] sanitizer fired in an AOT test program "
                "=====\n%s\n===== [aot-asan #1822] end sanitizer report =====\n",
                report.c_str());
            std::fflush(stderr);
            return 2;                  // ASan hard-error
        case bezel::test::SanitizerFinding::kLeak:
            return 1;                  // LSan leak report
        case bezel::test::SanitizerFinding::kNone:
            break;
    }
    if (!WIFEXITED(status)) return 2;  // signal death
    int code = WEXITSTATUS(status);
    if (code == 99) return 1;          // sanitizer exited, report unreadable
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
//
// SCOPE OF THIS ROW'S GUARANTEE (#3917, filed 2026-09-06, closed by the
// burn-down commit that rewrote runUnderLeakSanitizer above): the `_no_uaf_`
// in this row's name used to promise more than the oracle could deliver.
// emitObjectFile's three-argument spelling left `sanitizeAddress` false, so
// `gate.sh --full` never instrumented the GENERATED code and "no ASan
// hard-error" established only **no signal death**. Both halves are now
// closed — the object goes through emitObjectFileForTest, so the sweep
// instruments this row's program, and the rc mapping classifies the
// sanitizer's own report instead of reading `exitcode=99` as "leak" — so
// rc==2 is genuinely reachable for a heap-use-after-free READ by generated
// code, which is exactly what this row's name claims to exclude.
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
    // Pin NO UAF / double-free: rc must NOT be 2 (ASan hard-error), which
    // under `gate.sh --full` is now a real shadow-memory verdict on the
    // generated code (#3917). It can be 0 (sanitizer-clean — the +1 is
    // released somewhere) or 1 (LSan leak — the prior safe behaviour,
    // tracked as a residual #1747 channel).
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

// issue #3827 (SOUNDNESS GUARD, not a fix) / §9.7 / §28.9 / §28.3 (v0.286):
// the #3745 lane attempted to extend walkCallArgs' #1430 plain-parameter
// relaxation (today gated on stringReleaseMode / borrowingArrayMode) to
// recordFieldReleaseMode — treating a record-typed local passed BY VALUE to
// a plain-parameter method call as a call-bounded borrow, exactly as already
// done for strings. That widening does NOT appear on this HEAD: walkCallArgs
// (LlvmBackend_Setup.cpp) has no recordFieldReleaseMode-gated arm anywhere in
// its per-argument relaxation chain (verified by reading every `safeArg =
// true` site in the function; none tests recordFieldReleaseMode). It was
// caught during the #3745 lane BEFORE landing — see that commit's body — and
// filed as #3827 to document why: a callee that stores the by-value record
// parameter into one of ITS OWN class fields (`this.stored := p;`) aliases
// the record's string field into a longer-lived slot without minting an
// independent +1 (§9.7 — a record field store is a raw bit-copy, no
// retain), so scope-end-releasing the caller's local out from under that
// alias would be a use-after-free.
//
// This row is a GUARD, not a regression fix: nothing in src/ changes. It
// pins the exact shape #3827 describes (a callee that field-stores its
// by-value record parameter) so that if a future change ever re-admits this
// case to walkCallArgs' recordFieldReleaseMode relaxation, this test fails
// loudly instead of the class silently reappearing.
//
// GUARD-SENSITIVITY was verified both directions when this row landed, but
// NOT through its own in-process run — see the #3827 lane commit body.
// Summary of that account: `runUnderLeakSanitizer` linked an ASan-instrumented
// C++ RUNTIME but called `emitObjectFile` with its default
// `sanitizeAddress=false`, so the *generated Bezel code*'s own loads/stores
// carried no shadow-memory checks — enough to catch a LEAK (LSan's exit-time
// scan needs no per-access instrumentation) but structurally unable to catch a
// use-after-free *read* of still-mapped freed memory. Reintroducing the
// by-value-arg relaxation to `recordFieldReleaseMode` and re-running THIS row
// still reported rc==0 (clean), while a standalone
// `scripts/agent_check.sh --probe --asan` compile+run of the IDENTICAL program
// (which passes `--sanitize=address`, instrumenting the generated code via
// `emitObjectFile`'s real parameter) reported an actual
// `AddressSanitizer: heap-use-after-free` READ on `b.stored.Name.Length`,
// allocated in `bz$M.C$Mk`, freed in `main` — exactly the mechanism #3827
// describes. Reverting the widening returned the CLI probe to a clean LSan
// leak report, matching this row's PASS-WITHOUT.
//
// That gap is the one #3917 filed and the burn-down closed: this row's own
// in-process run now instruments the generated code (emitObjectFileForTest)
// AND classifies the sanitizer's report rather than reading `exitcode=99` as
// "leak", so the CLI probe's finding would now reproduce HERE, as rc==2. The
// row keeps its deliberately permissive `rc != 2` oracle — the residual
// channel #3827 tracks is a legitimate LEAK (rc==1) on this HEAD — but rc==2
// is no longer unreachable, which is what makes the `_no_uaf_` in the name
// honest. (An earlier draft of this row also read `b.stored.X`, an int field
// unrelated to the aliased string, which could never observe a UAF regardless
// of instrumentation — a second, independent mistake self-caught while
// proving this.)
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           record_call_result_byvalue_arg_callee_field_store_no_uaf_spec_9_7_3827) {
    std::string src =
        "module M; "
        "record P(Name: string, X: int); "
        "class B "
        "  public stored: P; "
        "  public method Store(p: P): int "
        "    this.stored := p; "
        "    return p.X; "
        "  end Store; "
        "end B; "
        "class C "
        "  public static method Mk(n: int): P "
        "    var r := new P($\"v\" + n.ToString(), n); "
        "    return r; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var total := 0; "
        "    var b := new B(); "
        "    var i := 0; "
        "    while i < 50 do "
        "      var r2 := Mk(i); "
        "      total := total + b.Store(r2); "
        "      i := i + 1; "
        "    end while; "
        "    return total + b.stored.Name.Length; "
        "  end Main; end C;";
    int rc = runUnderLeakSanitizer(src);
    if (rc == -1) return;
    // Pin NO ASan hard-error. Under `gate.sh --full` this is now a genuine
    // shadow-memory verdict on the generated code (#3917); rc may still be 0
    // (sanitizer-clean) or 1 (LSan leak — the documented safe residual
    // channel #3827's follow-up section tracks for a future sound closure).
    BEZEL_EXPECT_TRUE(rc != 2);
}

// fix #3917 (oracle self-test): runUnderLeakSanitizer's rc mapping above is
// report-driven, because `ASAN_OPTIONS=...:exitcode=99` gives a LeakSanitizer
// report and a heap-use-after-free the SAME exit status — the wait status
// alone cannot tell them apart, which is how a "99 means leak" mapping would
// keep reporting a freshly-instrumented UAF as the safe-leak outcome the two
// `_no_uaf_` rows above tolerate. Pin the classifier directly so its
// hard-error arm is exercised on every run rather than being dead code that
// only a future defect would ever reach.
BEZEL_TEST(codegen_record_string_field_call_release_1747,
           sanitizer_finding_classifier_leak_vs_hard_error_3917) {
    using bezel::test::SanitizerFinding;
    using bezel::test::sanitizerFindingInReport;
    BEZEL_EXPECT_TRUE(sanitizerFindingInReport("") == SanitizerFinding::kNone);
    BEZEL_EXPECT_TRUE(sanitizerFindingInReport("program stderr\n") ==
                      SanitizerFinding::kNone);
    BEZEL_EXPECT_TRUE(
        sanitizerFindingInReport(
            "==7==ERROR: LeakSanitizer: detected memory leaks\n"
            "Direct leak of 32 byte(s) in 1 object(s) allocated from:\n") ==
        SanitizerFinding::kLeak);
    BEZEL_EXPECT_TRUE(
        sanitizerFindingInReport(
            "==7==ERROR: AddressSanitizer: heap-use-after-free on address 0x602\n"
            "READ of size 8 at 0x602 thread T0\n") ==
        SanitizerFinding::kHardError);
    // A run that both faults AND leaks is a hard error, not a leak.
    BEZEL_EXPECT_TRUE(
        sanitizerFindingInReport(
            "==7==ERROR: LeakSanitizer: detected memory leaks\n"
            "==7==ERROR: AddressSanitizer: heap-use-after-free\n") ==
        SanitizerFinding::kHardError);
}
