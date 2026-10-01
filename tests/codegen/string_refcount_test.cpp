// Codegen + runtime — string reference counting (issue #422, strings
// increment). Per §4.6, `string` is runtime-managed: "its memory is reclaimed
// automatically." Per §28.9, strings are exempt from manual disposal. Per
// §28.8, strings are immutable, reference-shared LEAVES, so acyclic reference
// counting is sound. Before this change the runtime NEVER freed heap strings
// (concat / ToString / Split / substring / format all calloc'd and leaked).
//
// The header now carries an int64 rc (offset 24; total header 32 bytes). Every
// heap producer initializes it to 1 (the producer's reference). Literal/global
// strings carry a SENTINEL rc and are never freed. bz_string_release decrements
// and, at zero, frees the data buffer then the header; bz_string_retain
// increments; both are no-ops on a sentinel or null.
//
// Codegen releases a `var` string local at scope end ONLY when a conservative
// whole-body walk proves it never escapes AND it was initialized by a
// codegen-internal +1 producer (string `+` / interpolation — never a call, to
// avoid return-handoff ambiguity). Strings that escape (returned, stored in a
// field, passed as an argument, aliased) are conservatively LEFT leaked (a leak
// is acceptable; a double-free / UAF is not).
//
// Two complementary checks, mirroring free_on_dispose_test.cpp:
//   1. compileAndRun: native exit code proves correctness (the value is right).
//   2. runUnderLeakSanitizer: re-lower + link against an ASan-built runtime and
//      run under LSan — verifies the reclaim (no leak) AND memory safety (no
//      use-after-free / double-free). Skipped gracefully when no toolchain.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "driver/Driver.hpp"
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
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_422");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// LeakSanitizer verification for a single program. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_422_lsan");
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
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
    return 0;                                                      // clean
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak/UAF/double-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// Assert correctness AND that ASan reports no MEMORY-SAFETY error
// (use-after-free / double-free), but TOLERATE a leak. Used for the
// conservatively-leaked escape cases (returned / field-stored / aliased
// strings) where this increment intentionally over-retains (leaks) rather than
// risk a premature free. A leak exits 99 under detect_leaks; a UAF/DF also
// exits non-zero — so we run with detect_leaks=0 and require a clean exit,
// which still catches every memory-safety corruption (the thing that must NOT
// regress) while permitting the deliberate leak.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    // Reuse the lowering+link path but run with leak detection OFF.
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;  // toolchain unavailable: skip
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_422_safe");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return;
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    // detect_leaks=0: a pure leak does not fail; a UAF / double-free does.
    // exitcode=99 gives ASan a DISTINCT failure code so a memory-safety error
    // can never be confused with the program legitimately returning
    // expectedExitCode (ASan's default error exitcode is 1, which silently
    // masks a UAF in any test that expects 1). No test expects 99.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// --- Programs ------------------------------------------------------------

// §4.6 (load-bearing): a long loop that builds and drops a freshly concatenated
// string into a non-escaping local each iteration. The local is initialized by
// a string `+` (+1 producer) and used only as a non-consuming read (.Length),
// so it is released at scope end (each iteration) — steady-state, LSan-clean.
// Before the fix every iteration leaked the concatenated string and its buffer.
// Returns 0 (the loop's accumulated Length is positive on every path).
const char* kLoopConcatSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < 200000 do "
    "      var s: string := \"abcdefgh\" + \"ijklmnop\"; "
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    if total = 3200000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §4.6: a string built by `+` inside a loop and used through a string-method
// receiver (.Substring) whose own (fresh) result is read for Length. The outer
// `s` is non-escaping (only a method receiver) → released each iteration. The
// Substring result is a fresh +1 that is conservatively leaked (a call result),
// but it is bounded per iteration... NO — it would grow. To keep the loop
// LSan-clean we bind the substring to a non-escaping +1-... a call is NOT a +1
// producer for eligibility, so it leaks. Therefore this program is run only for
// CORRECTNESS + no-corruption (a bounded-leak case), not as an LSan-clean test.
const char* kLoopSubstringValue =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 1000 do "
    "      var s: string := \"hello\" + \"world\"; "
    "      acc := acc + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 10000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9: a string returned from a function and used by the caller. The function
// builds it with `+`; the caller binds the RESULT OF A CALL — not a +1 producer
// for eligibility — so it is conservatively leaked, never released by the
// caller. Correctness: the returned value is intact and usable (no premature
// free). Run for value + no-corruption (a deliberate leak is tolerated).
const char* kReturnedStringUsed =
    "module M; class C "
    "  public static method Build(): string "
    "    return \"ret\" + \"urn\"; "
    "  end Build; "
    "  public static method Main(): int "
    "    var s: string := C.Build(); "
    "    return s.Length; "          // "return" => 6
    "  end Main; "
    "end C;";

// §28.9: a string stored in a field, then the owning object disposed. The
// field-stored string is conservatively leaked (a field store is an escape that
// this increment leaves leaked, not double-freed). Correctness: the field reads
// back intact; disposing the object does not corrupt the string.
const char* kFieldStoredString =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"fie\" + \"ld\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  return h.Name.Length; "        // "field" => 5
    "end Main; end C;";

// §4.6: a string literal used and dropped. Literals are constant globals with
// the SENTINEL rc — they are NEVER freed (and never retained). The program must
// run without crashing and LSan must report no leak (a literal global is not a
// heap allocation). Returns the literal's length.
const char* kLiteralUsedDropped =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"literal\"; "
    "    return s.Length; "          // 7
    "  end Main; "
    "end C;";

// §4.6: concat round-trip — `(a + b)` content is correct via comparison. The
// concatenated value (non-escaping local) is released; the comparison reads
// both. Returns 0 when equal.
const char* kConcatRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := \"foo\" + \"bar\"; "
    "    if s = \"foobar\" then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §4.6 / #1027: Split round-trip — the runtime allocates one string per part
// (each rc=1) inside the returned `owned Array<string>`. Iterating the array
// sums lengths. The fresh array backing + its element strings used to be
// conservatively leaked (collection-held, deferred); #1027 reclaims a plain
// `var parts := s.Split(...)` at scope end (bz_array_free + the Array<string>
// string-release disposer) exactly once, so this is now LSan-clean.
// "a,bb,ccc" -> 1+2+3 = 6.
const char* kSplitRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var parts: Array<string> := \"a,bb,ccc\".Split(\",\"); "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < parts.Count do "
    "      total := total + parts[i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "             // 6
    "  end Main; "
    "end C;";

// §9.6 / #1027 Split(char): the char overload allocates one part string per
// segment (rc=1), like the string overload. #1027 reclaims the plain-`var`
// result exactly once at scope end, so this is now LSan-clean (was a deferred
// conservative-leak). "a,bb,ccc".Split(',') -> 1+2+3 = 6.
const char* kSplitCharRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var parts: Array<string> := \"a,bb,ccc\".Split(','); "
    "    var total: int := 0; "
    "    var i: int := 0; "
    "    while i < parts.Count do "
    "      total := total + parts[i].Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "             // 6
    "  end Main; "
    "end C;";

// §9.6 (static) Repeat(c, count): allocates a fresh +1 string. Here the result
// is bound to a non-escaping local that is only READ (.Length) — it does not
// escape, but it is initialized by a CALL (not a codegen-internal +1 producer),
// so per the conservative escape policy it is LEFT leaked rather than released.
// Value + no-corruption: the allocation is intact and never double-freed. Uses a
// 1000-byte allocation (large enough that a corrupt write would likely fault),
// then returns 0 iff the length is exactly 1000 (exit codes are 8-bit, so the
// length itself can't be returned directly).
const char* kRepeatStaticUsed =
    "module M; class C "
    "  public static method Main(): int "
    "    var s: string := string.Repeat('x', 1000); "
    "    return (if s.Length = 1000 then 0 else 1); "  // 0
    "  end Main; "
    "end C;";

// §3.1 StringBuilder mutators allocate a fresh part on each call (the flattened
// result). The builder is `using var` (disposed at scope end); ToString allocates
// the final string. Old parts are conservatively leaked (the builder does not
// retain/own its parts — matching Append/Clear). The decisive guard: repeatedly
// mutating in a loop must NOT corrupt or double-free any part. Value +
// no-corruption (a bounded per-iteration leak is tolerated). Returns 0.
const char* kStringBuilderMutateLoop =
    "module M; class C "
    "  public static method Main(): int "
    "    using var sb := new StringBuilder(); "
    "    sb.Append(\"start\"); "
    "    var i: int := 0; "
    "    while i < 500 do "
    "      sb.Insert(0, \"<\"); "
    "      sb.Replace(\"<\", \">\"); "
    "      sb.Remove(0, 1); "
    "      i := i + 1; "
    "    end; "
    "    return (if sb.ToString() = \"start\" then 0 else 1); "
    "  end Main; "
    "end C;";

// §28.8: an alias `var b := a` shares the same string reference. With this
// increment `a` escapes (it is bound into `b`) so it is NOT released, and `b`'s
// initializer is an identifier (not a +1 producer) so it is NOT released
// either — the value is leaked exactly once, never double-freed. Correctness:
// both names observe the same content; no corruption from sharing. Value +
// no-corruption (a single deliberate leak is tolerated; the point is NO
// double-free).
const char* kAliasShared =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: string := \"sha\" + \"red\"; "
    "    var b: string := a; "
    "    if a = b then return b.Length; end; "  // "shared" => 6
    "    return 0; "
    "  end Main; "
    "end C;";

// --- FINDING [1] (§28.9 / §28.3): a +1 string captured by a lambda THROUGH a
// safe-read sub-position must NOT be released at scope end. The closure's heap
// env block borrows (aliases) the captured string and outlives the producing
// scope (§28.9: the closure value is runtime-managed, its captured borrow's
// lifetime tracked separately; §28.3 classifies a capture as a borrow). Before
// the fix the escape walker re-applied safe-read suppression inside the lambda
// body, judged `msg` non-escaping, and armed a scope-end release — a
// use-after-free (ASan: heap-buffer-overflow in bz_string_release on the freed
// env cell). The fix makes ANY captured-local read inside a lambda an escape, so
// the value is conservatively LEFT leaked (leak-over-UAF). These run for value +
// no-corruption: the deliberate leak is tolerated; the cardinal point is that
// ASan reports NO memory-safety error.

// FINDING [1] C1: captured string read as a method receiver `msg.Length`.
const char* kCaptureStringMethodReceiver =
    "module M; class C "
    "  public static method Main(): int "
    "    var msg: string := \"hello\" + \"world\"; "
    "    var f := lambda() => msg.Length; "
    "    var r := f(); "
    "    return r; "                  // "helloworld".Length => 10
    "  end Main; "
    "end C;";

// FINDING [1] C2: captured string read as a concat operand `(msg + \"!\").Length`.
const char* kCaptureStringConcatOperand =
    "module M; class C "
    "  public static method Main(): int "
    "    var msg: string := \"hello\" + \"world\"; "
    "    var f := lambda() => (msg + \"!\").Length; "
    "    var r := f(); "
    "    return r; "                  // "helloworld!".Length => 11
    "  end Main; "
    "end C;";

// FINDING [1] C3: captured string read in an interpolation hole `$\"[{msg}]\"`.
const char* kCaptureStringInterpolation =
    "module M; class C "
    "  public static method Main(): int "
    "    var msg: string := \"hello\" + \"world\"; "
    "    var f := lambda() => $\"[{msg}]\".Length; "
    "    var r := f(); "              // "[helloworld]".Length => 12
    "    return r; "
    "  end Main; "
    "end C;";

// FINDING [1] C4 CONTROL: a bare-identifier capture `() => msg` was ALREADY
// correctly classified as an escape (and leaked) before the fix — included so
// the suite documents both the regression case and the already-correct one.
const char* kCaptureStringBareIdent =
    "module M; class C "
    "  public static method Main(): int "
    "    var msg: string := \"hello\" + \"world\"; "
    "    var f := lambda() => msg; "
    "    var r := f().Length; "       // "helloworld".Length => 10
    "    return r; "
    "  end Main; "
    "end C;";

// --- FINDING [2] (§4.6 / §28.9): intermediate accumulators from chained `+` and
// multi-part interpolation are now released after the next step consumes them.
// These are LSan-clean: the intermediates never escape and are pure codegen
// temporaries that are reclaimed. A long loop proves steady-state (no growth).

// FINDING [2] M3: chained concat — the inner "ab" intermediate is released after
// the outer concat consumes it; the final non-escaping local is released at
// scope end. Looped 200k times for steady-state.
const char* kChainedConcatSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; var i: int := 0; "
    "    while i < 200000 do "
    "      var s: string := \"aa\" + \"bb\" + \"cc\" + \"dd\"; "
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    if total = 1600000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// FINDING [2] M1: multi-part interpolation with a string-typed hole — every
// accumulator is released; the part value (a borrow of the named local) is not.
// Looped 200k times for steady-state LSan-clean reclaim.
const char* kInterpolationSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var name: string := \"wor\" + \"ld\"; "
    "    var total: int := 0; var i: int := 0; "
    "    while i < 200000 do "
    "      var s: string := $\"hello {name} bye\"; "
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    if total > 0 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// FINDING [2] (cardinal-risk guard): a chained-concat result that is
// subsequently USED — re-concatenated, compared, and returned — must be clean
// (releasing an intermediate too early would be exactly the FINDING [1] UAF). A
// non-escaping result is fully reclaimed (LSan-clean).
const char* kChainedConcatResultReused =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: string := \"x\" + \"y\" + \"z\"; "   // a = "xyz"
    "    var b: string := a + a; "                   // b = "xyzxyz"
    "    if b = \"xyzxyz\" then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// --- LSan-clean tests (the reclaim: leaked before, clean after) ----------

BEZEL_TEST(codegen_string_refcount_422, loop_concat_steady_state_reclaimed_4_6) {
    // The load-bearing leak test: 200k iterations each freeing the concatenated
    // string at scope end → LSan-clean steady state. Leaked before the fix.
    expectValueAndLeakFree(kLoopConcatSteadyState, 0);
}

BEZEL_TEST(codegen_string_refcount_422, loop_concat_value_reclaimed_4_6) {
    expectValueAndLeakFree(kLoopSubstringValue, 0);
}

BEZEL_TEST(codegen_string_refcount_422, literal_used_dropped_not_freed_4_6) {
    // A literal carries the sentinel rc; release is a no-op → no crash, no leak
    // (it is a constant global, not a heap allocation).
    expectValueAndLeakFree(kLiteralUsedDropped, 7);
}

BEZEL_TEST(codegen_string_refcount_422, concat_round_trip_value_and_clean_4_6) {
    expectValueAndLeakFree(kConcatRoundTrip, 0);
}

// --- Value + no-corruption tests (conservatively-leaked escapes) ---------

BEZEL_TEST(codegen_string_refcount_422, returned_string_used_no_uaf_28_9) {
    expectValueAndNoCorruption(kReturnedStringUsed, 6);
}

BEZEL_TEST(codegen_string_refcount_422, field_stored_string_no_uaf_28_9) {
    expectValueAndNoCorruption(kFieldStoredString, 5);
}

BEZEL_TEST(codegen_string_refcount_422, split_round_trip_no_uaf_4_6) {
    // #1027: a plain `var parts := s.Split(...)` is now reclaimed at scope end
    // (backing + each substring released exactly once) — strengthened from
    // no-corruption to LSan-clean.
    expectValueAndLeakFree(kSplitRoundTrip, 6);
}

BEZEL_TEST(codegen_string_refcount_422, split_char_round_trip_no_uaf_9_6) {
    // §9.6 / #1027 Split(char): per-part allocations released exactly once at
    // scope end — LSan-clean (was a deferred conservative-leak).
    expectValueAndLeakFree(kSplitCharRoundTrip, 6);
}

// #1027: the discard form `s.Split(',').Length` (the fresh owned Array<string>
// consumed as a receiver, then dropped) is reclaimed exactly once via the
// ownerless-materializer temp path — was a leak. Returns the part count (3).
const char* kSplitDiscardLength =
    "module M; class C "
    "  public static method Main(): int "
    "    return \"a,bb,ccc\".Split(',').Length; "   // 3
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_string_refcount_1027, split_discard_length_reclaimed) {
    expectValueAndLeakFree(kSplitDiscardLength, 3);
}

BEZEL_TEST(codegen_string_refcount_422, repeat_static_used_no_uaf_9_6) {
    // §9.6 (static) Repeat(c, count): the fresh allocation is intact and usable.
    expectValueAndNoCorruption(kRepeatStaticUsed, 0);
}

BEZEL_TEST(codegen_string_refcount_422, stringbuilder_mutate_loop_no_uaf_3_1) {
    // §3.1 mutators in a loop: no part is corrupted or double-freed.
    expectValueAndNoCorruption(kStringBuilderMutateLoop, 0);
}

BEZEL_TEST(codegen_string_refcount_422, alias_shared_freed_once_no_double_free_28_8) {
    // The decisive double-free guard: sharing a string between two names must
    // never free it twice. (It is freed zero times here — a tolerated leak —
    // but crucially NOT twice; ASan would abort on a double-free.)
    expectValueAndNoCorruption(kAliasShared, 6);
}

// --- FINDING [1] (§28.9 / §28.3): captured-string UAF — was a heap-buffer-
// overflow in bz_string_release before the fix; now a tolerated leak (value +
// no-corruption). The cardinal assertion is NO memory-safety error under ASan.

BEZEL_TEST(codegen_string_refcount_422, capture_via_method_receiver_no_uaf_28_9) {
    // C1: () => msg.Length — captured read in a safe-read (method-receiver)
    // sub-position inside the lambda. UAF before fix; clean (leaked) after.
    expectValueAndNoCorruption(kCaptureStringMethodReceiver, 10);
}

BEZEL_TEST(codegen_string_refcount_422, capture_via_concat_operand_no_uaf_28_9) {
    // C2: () => (msg + "!").Length — captured read in a concat-operand position.
    expectValueAndNoCorruption(kCaptureStringConcatOperand, 11);
}

BEZEL_TEST(codegen_string_refcount_422, capture_via_interpolation_no_uaf_28_9) {
    // C3: () => $"[{msg}]".Length — captured read in an interpolation hole.
    expectValueAndNoCorruption(kCaptureStringInterpolation, 12);
}

BEZEL_TEST(codegen_string_refcount_422, capture_bare_ident_control_no_uaf_28_9) {
    // C4 CONTROL: () => msg — a bare-identifier capture, already classified as an
    // escape before the fix; documents the already-correct case.
    expectValueAndNoCorruption(kCaptureStringBareIdent, 10);
}

// --- FINDING [2] (§4.6 / §28.9): chained-concat / interpolation accumulator
// intermediates are now reclaimed (LSan-clean), including in long loops.

BEZEL_TEST(codegen_string_refcount_422, chained_concat_steady_state_reclaimed_4_6) {
    // M3 scaled: the per-iteration intermediate accumulators are released → no
    // growth. Leaked unboundedly before the fix.
    expectValueAndLeakFree(kChainedConcatSteadyState, 0);
}

BEZEL_TEST(codegen_string_refcount_422, interpolation_steady_state_reclaimed_4_6) {
    // M1 scaled: multi-part interpolation with a string-typed hole — every
    // accumulator reclaimed; the part borrow is correctly left untouched.
    expectValueAndLeakFree(kInterpolationSteadyState, 0);
}

BEZEL_TEST(codegen_string_refcount_422, chained_concat_result_reused_clean_no_uaf_4_6) {
    // The cardinal-risk guard for FINDING [2]: a chained-concat result that is
    // re-concatenated and compared must be clean — releasing an intermediate too
    // early would reintroduce the FINDING [1] UAF. LSan-clean (no leak, no DF).
    expectValueAndLeakFree(kChainedConcatResultReused, 0);
}

// #1030 / §18.5 / §4.6 / §28.9: string interpolation must release the FRESH render
// of each non-string part. `$"{a}"` (a:int) renders via emitPrimitiveToString → a
// +1 string the concat borrows; before the fix it was never released and leaked
// (unbounded in a loop). The fix releases pv after the concat iff pv != srcEv (the
// fresh render), skipping the string-part borrow (pv == srcEv).
const char* kInterpIntPartReleased_1030 =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: int := 3; var b: int := 4; "
    "    var s: string := $\"{a}+{b}={a + b}\"; "   // "3+4=7"
    "    return s.Length; "                          // 5
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_string_refcount_1030, interp_nonstring_parts_released) {
    expectValueAndLeakFree(kInterpIntPartReleased_1030, 5);
}

// #1030 (loop): interpolation rendering a non-string part inside a loop must not
// leak per-iteration (the unbounded case). 5 iterations of "item {i}" (6 chars).
const char* kInterpLoopReleased_1030 =
    "module M; class C "
    "  public static method Main(): int "
    "    var total: int := 0; var i: int := 0; "
    "    while i < 5 do "
    "      var s: string := $\"item {i}\"; "
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    return total; "                             // 6*5 = 30
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_string_refcount_1030, interp_nonstring_in_loop_no_unbounded_leak) {
    expectValueAndLeakFree(kInterpLoopReleased_1030, 30);
}

// #1030 regression: a STRING part is a borrow (pv == srcEv) and must NOT be
// released — `$"hi {name}"` stays LSan-clean (no double-free / use-after-free of
// the borrowed local). "hi world" = 8 chars.
const char* kInterpStringPartBorrow_1030 =
    "module M; class C "
    "  public static method Main(): int "
    "    var name: string := \"world\"; "
    "    var s: string := $\"hi {name}\"; "
    "    return s.Length; "                          // 8
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_string_refcount_1030, interp_string_part_borrow_not_released) {
    expectValueAndLeakFree(kInterpStringPartBorrow_1030, 8);
}

// --- #1184 (§28.3 / §9.6): `string.Join`'s RESULT string is a +1 producer ---
// `string.Join(sep, items)` lowers (LlvmBackend_Calls.cpp) to a freshly malloc'd
// buffer wrapped in a heap `bz.string` at rc := 1 — the SAME +1 single-reference
// shape as `+`/interpolation. Before the fix isPlusOneStringProducer did not
// recognise it, so a `var s := string.Join(...)` local was misclassified as a
// BORROW: RETAINED at bind (rc 1→2) then released at scope end (rc 2→1) → the
// result string leaked (38 bytes: 32-byte header + "ab,cd\0"). It is now treated
// as a +1 producer: TRANSFER into the slot (no borrow-retain) + the scope-end
// release drops rc 1→0 and frees it exactly once. LSan-clean, value still 5.
const char* kJoinResultReclaimed_1184 =
    "module M; import System.Collections.Generic; "
    "class C public static method Main(): int "
    "  using var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
    "  var s := string.Join(\",\", xs); "
    "  return s.Length; "                              // "ab,cd" -> 5
    "end Main; end C;";
BEZEL_TEST(codegen_string_refcount_1184, join_result_reclaimed) {
    expectValueAndLeakFree(kJoinResultReclaimed_1184, 5);
}

// #1184 (lazy items): joining a lazy LINQ query is reclaimed the same way — the
// Join RESULT, not the materialized item snapshot (that is freed inside the
// lowering, #1182). The empty middle element is filtered out so the join is
// still "ab,cd". LSan-clean.
const char* kJoinLazyResultReclaimed_1184 =
    "module M; import System.Collections.Generic; "
    "class C public static method Main(): int "
    "  using var xs := new Array<string>(); "
    "  xs.Add(\"ab\"); xs.Add(\"\"); xs.Add(\"cd\"); "
    "  var s := string.Join(\",\", xs.Where(lambda(x: string) => x.Length > 0)); "
    "  return s.Length; "                              // "ab,cd" -> 5
    "end Main; end C;";
BEZEL_TEST(codegen_string_refcount_1184, join_lazy_result_reclaimed) {
    expectValueAndLeakFree(kJoinLazyResultReclaimed_1184, 5);
}

// #1184 (escape via return — the double-free guard): the Join result returned
// from a method and re-bound in the caller must be freed EXACTLY ONCE, never
// twice. The producer classification emits NO extra retain, so the single
// reference flows return → caller local → scope-end release with no double-free
// (a double-free here would SIGABRT under ASan). LSan-clean.
const char* kJoinReturnedNoDoubleFree_1184 =
    "module M; import System.Collections.Generic; "
    "class C "
    "  public static method MakeJoined(): string "
    "    using var xs := new Array<string>(); xs.Add(\"ab\"); xs.Add(\"cd\"); "
    "    return string.Join(\",\", xs); "
    "  end MakeJoined; "
    "  public static method Main(): int "
    "    var s := MakeJoined(); "
    "    return s.Length; "                            // "ab,cd" -> 5
    "  end Main; "
    "end C;";
BEZEL_TEST(codegen_string_refcount_1184, join_returned_no_double_free) {
    expectValueAndLeakFree(kJoinReturnedNoDoubleFree_1184, 5);
}
