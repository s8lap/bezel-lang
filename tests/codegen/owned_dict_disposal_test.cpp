// Codegen — scope-end disposal of an OwningDictionary<K,V>'s surviving values
// (issue #394, stdlib §4.9). Companion coverage for OwningSet<T> (stdlib §4.10),
// whose class elements are array-backed and already disposed via #362.
//
// stdlib §4.9: "Owning hash-based map. Owns its values; keys are value-like."
// And: "There is no `DisposeElements()` on `OwningDictionary<K, V>` — the
// dictionary's `Dispose()` cascade handles every value disposal." Keys are NOT
// owned ("`OwningDictionary` does not support owned keys"), so only the values
// (V, constrained `V: class`) are disposed at scope end.
//
// stdlib §4.10: "`OwningSet<T>` ... where T: class" — owns its elements; each
// surviving element is disposed once at scope end.
//
// The bug (#394): the scope-end `using var` disposal of an OwningDictionary was
// not wired at all — neither the backing hash table nor its surviving values
// were freed. #388 wired the SETTER's disposal of a DISPLACED value
// (bz_dict_set_*_disposing); this file covers the values still present in the
// table when the `using var` scope ends, which must be disposed exactly once.
//
// Two complementary check kinds (mirroring undisposed_leak_test.cpp):
//   * destructor-count JIT tests (always run): a resource class's destructor
//     bumps a static counter, observable through the int exit code — proves the
//     scope-end disposal now runs each surviving value's destructor exactly the
//     right number of times.
//   * LeakSanitizer native tests (run only with a sanitizer toolchain): lower
//     to an object file, link against an ASan-instrumented runtime, run under
//     LSan — proves the values' blocks are reclaimed with NO leak AND NO
//     double-free / use-after-free (load-bearing for the displaced+surviving
//     case, where the displaced value must NOT be disposed twice).

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

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification (mirrors undisposed_leak_test.cpp). Lowers `source`
// through the full front-end to an object file, links against an
// ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no problem of the kinds being detected).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// Uses the memoized ASan runtime object (compiled once per process).
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_394_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the destructor-count (always) and, if the toolchain is present, that
// LeakSanitizer reports no leak / no double-free / no use-after-free.
void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// A resource class R (destructor bumps R._n) and a Main holding the given body.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

// --- OwningDictionary<string, R> — string-key fast path (§4.9). -------------

// Two owned values added via .Add(k, move v); both survive to scope end. The
// dictionary's Dispose() cascade disposes both values exactly once. Encoded:
// <count inside the scope, before exit> * 10 + <count after scope end>. Inside
// the scope nothing is disposed (0); at scope end both are disposed (2) → 02.
// Before #394 the values leaked and the count stayed 0 → 00.
const char* kDictStrTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  d.Add(\"b\", move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// The displaced + surviving combination (the no-double-free case). Three keys
// are written via the indexer setter (§4.9 set(owned)); key "a" is written
// twice, so its first value is DISPLACED and disposed by the #388 setter path.
// At the displacement point R._n == 1. At scope end the THREE surviving values
// ("a"-second, "b", "c") are disposed → R._n == 4 total. The displaced value is
// already gone from the table, so it is NOT disposed again. Encoded:
// <count after displacement> * 10 + <count after scope end> = 1*10 + 4 = 14.
// Before #394: only the displaced one was disposed → 1*10 + 1 = 11.
const char* kDictStrDisplacedPlusSurvivors =
    "var afterDisplace := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d[\"a\"] := move (new R()); "
    "  d[\"b\"] := move (new R()); "
    "  d[\"c\"] := move (new R()); "
    "  d[\"a\"] := move (new R()); "   // displaces "a"-first (disposed here)
    "  afterDisplace := R._n; "
    "end; "
    "return afterDisplace * 10 + R._n;";

// --- OwningDictionary<int, R> — generic-key path (§4.9 / D3). ---------------

// An int key uses the generic-key runtime (bz_dict_*_gen). The owned values are
// still disposed at scope end. Two survivors → 02 (0 inside, 2 after).
const char* kDictIntTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<int, R>(); "
    "  d.Add(1, move (new R())); "
    "  d.Add(2, move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// --- OwningSet<R> — array-backed class elements (§4.10, already via #362). ---

// OwningSet<T> where T: class is array-backed (a class element is not a generic-
// key-eligible value type), so its scope-end disposal already cascades into each
// element via the OwningArray element-disposer (#362). This test pins that
// behavior so the dict fix does not regress it. Two elements → 02.
const char* kSetTwoSurvivors =
    "var inside := -1; "
    "do "
    "  using var s := new OwningSet<R>(); "
    "  s.Add(move (new R())); "
    "  s.Add(move (new R())); "
    "  inside := R._n; "
    "end; "
    "return inside * 10 + R._n;";

// --- #396: explicit .Dispose() on an OwningDictionary disposes its values. ---

// #396 / §4.9 + §28.2: an explicit `d.Dispose()` on an OwningDictionary<K,V>
// (V a class) must cascade disposal into its surviving values — exactly as the
// scope-end (#394) path does. Here both values are disposed BY the explicit
// call (count 2 read AFTER it, INSIDE the scope), and the using-var's scope-end
// disposal is a NO-OP (§28.2 idempotency: the first Dispose disposed; the
// second is skipped), so the count is still 2 after the block → 2*10 + 2 = 22.
//
// This is the exactly-once interaction the issue calls out: explicit Dispose
// followed by scope-end disposal of the same `using var`. Before #396 the
// explicit call passed a NULL value-disposer (values leaked → inside count 0)
// AND then bz_dict_free ran a SECOND time on the freed table at scope end (a
// double-free / crash). The fix disposes the values once and nulls the slot so
// the scope-end bz_dict_free reloads null and no-ops. The LSan leg is the
// load-bearing check: no leak, no double-free, no use-after-free.
const char* kDictExplicitDispose =
    "var inside := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  d.Add(\"b\", move (new R())); "
    "  d.Dispose(); "
    "  inside := R._n; "          // both disposed by the explicit call → 2
    "end; "                       // scope-end disposal is a no-op (slot nulled)
    "return inside * 10 + R._n;"; // 2*10 + 2 = 22

// #396 / §4.9: an explicit `this._d.Dispose()` inside a DESTRUCTOR disposes the
// owned dict field's surviving values once. An owned OwningDictionary FIELD is
// not auto-disposed by the destructor's field-disposal pass (that pass handles
// class objects and array-backed collections, not hash-backed dicts), so the
// explicit call in the destructor is the only disposal — it must dispose the
// value. The owner is a `using var`; at its scope end the destructor runs,
// disposes the dict's one value (R._n → 1), then the field-disposal pass leaves
// the (now self-freed) dict field alone. The count is read after the owner's
// scope → 1. Before #396 the value leaked (NULL value-disposer) → 0.
const char* kDictFieldDestructorDispose =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class Owner "
    "  private owned _d: OwningDictionary<string, R>; "
    "  public constructor() "
    "    this._d := new OwningDictionary<string, R>(); "
    "    this._d.Add(\"a\", move (new R())); "
    "  end constructor; "
    "  destructor this._d.Dispose(); end destructor; "
    "end Owner; "
    "class C public static method Main(): int "
    "  do using var o := new Owner(); end; "
    "  return R._n; "
    "end Main; end C;";

}  // namespace

// ---- OwningDictionary scope-end disposal (§4.9, #394) ---------------------

BEZEL_TEST(codegen_owned_dict_394, str_key_surviving_values_disposed_4_9) {
    // Both surviving values disposed once at scope end (none disposed inside) →
    // 0*10 + 2 = 2. Before #394 the values leaked → 0. LSan leg: no leak.
    expectDisposedAndLeakFree(prog(kDictStrTwoSurvivors), 2);
}

BEZEL_TEST(codegen_owned_dict_394, str_key_displaced_plus_surviving_no_double_free_4_9) {
    // #388 setter disposes the displaced "a"-first value (count 1 at the
    // displacement point); the three survivors are disposed at scope end
    // (count 4 total). The displaced value is NOT disposed twice → 1*10 + 4 =
    // 14. Before #394 the survivors leaked → 1*10 + 1 = 11. The LSan leg is the
    // load-bearing check that there is no double-free of the displaced value.
    expectDisposedAndLeakFree(prog(kDictStrDisplacedPlusSurvivors), 14);
}

BEZEL_TEST(codegen_owned_dict_394, int_key_surviving_values_disposed_4_9) {
    // Generic-key dictionary: the surviving owned values are disposed at scope
    // end exactly like the string-key path → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kDictIntTwoSurvivors), 2);
}

// ---- OwningSet scope-end disposal (§4.10) — regression pin ----------------

BEZEL_TEST(codegen_owned_set_394, class_elements_surviving_disposed_4_10) {
    // OwningSet<R> (R a class) is array-backed; its surviving elements are
    // disposed at scope end via the OwningArray element-disposer (#362). This
    // must remain true after the dict fix → 0*10 + 2 = 2.
    expectDisposedAndLeakFree(prog(kSetTwoSurvivors), 2);
}

// ---- #396: explicit .Dispose() disposes OwningDictionary values (§4.9, §28.2) -

BEZEL_TEST(codegen_owned_dict_396, explicit_dispose_disposes_values_exactly_once_4_9) {
    // Explicit d.Dispose() disposes both values (inside count 2); the using
    // var's scope-end disposal is a §28.2 no-op (slot nulled), so the count is
    // still 2 after the block → 22. Before #396: values leaked AND the table
    // was double-freed at scope end. The LSan leg is load-bearing — it confirms
    // exactly-once disposal with no double-free / use-after-free.
    expectDisposedAndLeakFree(prog(kDictExplicitDispose), 22);
}

BEZEL_TEST(codegen_owned_dict_396, explicit_dispose_of_owned_field_in_destructor_4_9) {
    // this._d.Dispose() in a destructor disposes the owned dict field's one
    // value exactly once → R._n == 1. Before #396 the value leaked (NULL value-
    // disposer) → 0. LSan leg: no leak, no double-free.
    expectDisposedAndLeakFree(kDictFieldDestructorDispose, 1);
}

// ---- #1214: OwningDictionary.Add disposes the rejected value on dup-key throw -
//
// §4.8 / §28.3: when OwningDictionary.Add is called with a key that is already
// present it must (a) throw ArgumentException and (b) dispose the transferred
// owned value — ownership was transferred to the call site and the value will
// not be inserted, so the disposer must run on the throw path or the value leaks.
//
// Encoding: `inside_throw * 10 + R._n` where `inside_throw` is R._n captured
// inside the catch arm (≥1 iff the rejected value was disposed on the throw
// path) and `R._n` is the final count after the using-var scope ends.
//
// Correct outcome: inside_throw == 1 (rejected R disposed), R._n == 2 (surviving
// first R disposed at scope end) → 12.
// Before #1214: the rejected R leaked, inside_throw == 0 → return 2 or crash.
// The LSan leg is load-bearing: it confirms exactly-once disposal with no leak.

const char* kOwnedDictStrDupKeyDisposesValue =
    "var inside_throw := -1; "
    "do "
    "  using var d := new OwningDictionary<string, R>(); "
    "  d.Add(\"a\", move (new R())); "
    "  try d.Add(\"a\", move (new R())); "
    "  catch ArgumentException => inside_throw := R._n; end; "
    "end; "
    "return inside_throw * 10 + R._n;";

// Same scenario with a generic (non-string) int key — exercises the
// bz_dict_contains_gen probe path instead of bz_dict_contains_str.
const char* kOwnedDictIntDupKeyDisposesValue =
    "var inside_throw := -1; "
    "do "
    "  using var d := new OwningDictionary<int, R>(); "
    "  d.Add(1, move (new R())); "
    "  try d.Add(1, move (new R())); "
    "  catch ArgumentException => inside_throw := R._n; end; "
    "end; "
    "return inside_throw * 10 + R._n;";

BEZEL_TEST(codegen_owned_dict_1214,
           str_key_dup_add_disposes_rejected_value_spec_4_8_28_3) {
    // String key: the rejected second R is disposed on the throw path
    // (inside_throw == 1 inside the catch), the surviving first R is disposed
    // at scope end (R._n == 2 after the block) → 12.
    // LSan leg: no leak (rejected value freed), no double-free (surviving value
    // freed exactly once at scope end).
    expectDisposedAndLeakFree(prog(kOwnedDictStrDupKeyDisposesValue), 12);
}

BEZEL_TEST(codegen_owned_dict_1214,
           int_key_dup_add_disposes_rejected_value_spec_4_8_28_3) {
    // Generic int key: same ownership guarantee via the bz_dict_contains_gen
    // probe path → inside_throw == 1, R._n == 2 after scope → 12.
    // LSan leg: load-bearing for the generic-key probe path.
    expectDisposedAndLeakFree(prog(kOwnedDictIntDupKeyDisposesValue), 12);
}
