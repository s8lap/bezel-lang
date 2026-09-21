// Codegen — scope-end disposal of a (borrowing) Dictionary<K, record>'s
// record-VALUE BOXES (issue #401, contained half). stdlib §4.8 `Dictionary<K,
// V>`; language spec §3.2 / §9 (records are value types).
//
// stdlib §4.8: `Dictionary<K, V>` is the BORROWING map; `V` is unconstrained, so
// the value type may be a record. A record value is too wide for the scalar
// inttoptr value-slot encoding, so the runtime heap-BOXES it (malloc + memcpy of
// its bytes) and stores the box pointer (see dict_record_value_indexer_test.cpp
// and the encode in LlvmBackend_{Calls,Assign}.cpp). That box is the dict's OWN
// storage — language spec §3.2 ("value types ... do not [carry a disposal
// obligation]") and §9 ("A record is a value type ... value semantics") mean
// there is nothing to *dispose* inside the box; the box is just heap memory to
// FREE at scope end.
//
// The bug (#401, contained half): the scope-end `using var` disposal of a
// borrowing Dictionary<K, record> called bz_dict_free with a NULL value-disposer
// (only owning OwningDictionary<K, class> passed one, via #394). bz_dict_free thus
// freed the backing table and its key bytes but NOT the record-value boxes →
// each surviving box leaked. The fix passes a value-box-FREE callback (a plain
// `free`, NO destructor — records are value types) for a borrowing
// Dictionary<K, record>, so each surviving box is reclaimed at scope end.
//
// No-double-free scoping (the critical invariant, mirroring #394):
//   * The box-free callback is for the value-BOX case (record/value-type values
//     in a plain Dictionary) ONLY. An OwningDictionary<K, class> still routes
//     through #394's owning-class value-disposer (dispose + free via the
//     object's vtable), NOT the box-free — its values are class objects, not
//     boxes, and must be disposed exactly once. The #394 regression guard below
//     pins that.
//   * A value box DISPLACED by the indexer setter (overwriting an existing key)
//     is freed at the displacement point (#401 setter box-free), so it is no
//     longer in the table at scope end — bz_dict_free frees only the surviving
//     box. No box is freed twice.
//
// Verification mirrors owned_dict_disposal_test.cpp (#394): a LeakSanitizer
// native leg that lowers to an object file, links against an ASan-instrumented
// runtime, and runs under LSan. The LSan leg is the load-bearing check — before
// the fix the value boxes leaked (LSan would report N leaked allocations); after
// it they are reclaimed with no leak and no double-free. The JIT exit-code leg
// pins the program's value-level behavior (the dict still works) so a broken
// fix that corrupted the table would also be caught.

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
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
static const std::filesystem::path& wtDir401() {
    return wtScratchDir("wt-401");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}


// LeakSanitizer verification (mirrors owned_dict_disposal_test.cpp). Lowers
// `source` through the full front-end to an object file, links against an
// ASan-instrumented runtime, runs under LSan. Returns:
//   0  — clean (no leak / no double-free / no use-after-free).
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDir401();
    std::string stem = unique_stem("bezel_401_lsan");
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
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the JIT exit code (value-level behavior) and, if the toolchain is
// present, that LeakSanitizer reports no leak / no double-free / no UAF.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no DF/UAF)
}

// A Point record (value type) and a Main holding the given body.
std::string prog(const std::string& mainBody) {
    return
        "module M; "
        "record Point(x: int, y: int); "
        "class C public static method Main(): int " + mainBody + " end Main; "
        "end C;";
}

// --- Borrowing Dictionary<string, Point> — string-key path (§4.8). ---------

// Three record values boxed and stored under fresh string keys; all three
// survive to scope end, where the dict's disposal frees each value box. The
// exit code reads a field of one survivor (the dict still works). Before #401
// the three boxes leaked at scope end; the LSan leg is the load-bearing check.
const char* kStrThreeSurvivors =
    "var sum := 0; "
    "do "
    "  using var d := new Dictionary<string, Point>(); "
    "  d[\"a\"] := new Point(1, 2); "
    "  d[\"b\"] := new Point(3, 4); "
    "  d[\"c\"] := new Point(5, 6); "
    "  sum := d[\"a\"].x + d[\"b\"].y + d[\"c\"].x; "  // 1 + 4 + 5 = 10
    "end; "
    "return sum;";

// --- Borrowing Dictionary<int, Point> — generic-key path (§4.8). -----------

// An int key routes through the generic-key runtime; the boxed record values
// are still freed at scope end. Two survivors. Before #401 both boxes leaked.
const char* kIntTwoSurvivors =
    "var sum := 0; "
    "do "
    "  using var d := new Dictionary<int, Point>(); "
    "  d[1] := new Point(10, 20); "
    "  d[2] := new Point(30, 40); "
    "  sum := d[1].x + d[2].y; "                       // 10 + 40 = 50
    "end; "
    "return sum;";

// --- Displaced + surviving (the no-double-free / no-leak case). ------------

// Key "a" is written twice via the indexer setter, so its FIRST box is
// DISPLACED (overwritten). The displaced box must be freed at the displacement
// point (so it does not leak) and must NOT be freed again at scope end (the
// table holds only the second box for "a"). Three keys remain live ("a"-second,
// "b", "c"); their boxes are freed once at scope end. The exit code reads the
// surviving "a" value to confirm the overwrite took. Before #401 every box
// (displaced + survivors) leaked; a naive fix that freed a displaced box twice
// would double-free — the LSan leg catches both.
const char* kStrDisplacedPlusSurvivors =
    "var v := 0; "
    "do "
    "  using var d := new Dictionary<string, Point>(); "
    "  d[\"a\"] := new Point(1, 1); "
    "  d[\"b\"] := new Point(2, 2); "
    "  d[\"c\"] := new Point(3, 3); "
    "  d[\"a\"] := new Point(9, 9); "   // displaces "a"-first (freed here)
    "  v := d[\"a\"].x; "               // reads the second box → 9
    "end; "
    "return v;";

// Generic-key displaced + surviving (int key path).
const char* kIntDisplacedPlusSurvivors =
    "var v := 0; "
    "do "
    "  using var d := new Dictionary<int, Point>(); "
    "  d[1] := new Point(1, 1); "
    "  d[2] := new Point(2, 2); "
    "  d[1] := new Point(7, 7); "       // displaces key-1-first (freed here)
    "  v := d[1].x; "                   // reads the second box → 7
    "end; "
    "return v;";

// --- Explicit .Dispose() on a borrowing record-valued dict (§4.8 / §28.2). -

// An explicit d.Dispose() frees the value boxes (and the table); the using
// var's scope-end disposal is a §28.2 no-op (the slot is nulled by the explicit
// path, so the scope-end bz_dict_free reloads null and returns). No box is
// freed twice. The value is read BEFORE the explicit Dispose.
const char* kStrExplicitDispose =
    "var v := 0; "
    "do "
    "  using var d := new Dictionary<string, Point>(); "
    "  d[\"a\"] := new Point(4, 5); "
    "  v := d[\"a\"].x + d[\"a\"].y; "  // 4 + 5 = 9 (read before Dispose)
    "  d.Dispose(); "                   // frees the box + table
    "end; "                             // scope-end disposal is a no-op
    "return v;";

// --- #394 regression guard: OwningDictionary<K, class> values disposed once. --

// An OwningDictionary<K, R> (R a class) must STILL dispose its class values
// exactly once at scope end via #394's owning-class value-disposer — NOT via
// the new value-box-free path (a class value is an object, not a record box).
// R's destructor bumps a static counter; two survivors are disposed once each
// → R._n == 2. A regression where the #401 box-free ran on these class values
// would double-free (LSan failure) or skip disposal (counter wrong).
const char* kOwnedDictClassValuesGuard =
    "module M; "
    "class R "
    "  public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; "
    "end R; "
    "class C public static method Main(): int "
    "  do "
    "    using var d := new OwningDictionary<string, R>(); "
    "    d.Add(\"a\", move (new R())); "
    "    d.Add(\"b\", move (new R())); "
    "  end; "
    "  return R._n; "                   // both disposed once each → 2
    "end Main; end C;";

}  // namespace

// ---- Borrowing Dictionary<K, record> value-box disposal (§4.8, #401) ------

BEZEL_TEST(codegen_dict_record_value_leak_401, str_key_survivors_boxes_freed_4_8) {
    // Three boxed record values, all surviving to scope end, are freed once
    // each. JIT exit code = 1 + 4 + 5 = 10. LSan leg: no leak (3 boxes were
    // leaked before #401), no double-free.
    expectValueAndLeakFree(prog(kStrThreeSurvivors), 10);
}

BEZEL_TEST(codegen_dict_record_value_leak_401, int_key_survivors_boxes_freed_4_8) {
    // Generic-key dictionary: boxed record values are freed at scope end exactly
    // like the string-key path. JIT exit code = 10 + 40 = 50. LSan leg: no leak.
    expectValueAndLeakFree(prog(kIntTwoSurvivors), 50);
}

BEZEL_TEST(codegen_dict_record_value_leak_401, str_key_displaced_plus_survivors_no_double_free_4_8) {
    // The displaced box is freed at the overwrite point; the three survivors are
    // freed once at scope end. JIT exit code reads the surviving "a" → 9. LSan
    // leg is load-bearing: no leak of the displaced box, no double-free of it.
    expectValueAndLeakFree(prog(kStrDisplacedPlusSurvivors), 9);
}

BEZEL_TEST(codegen_dict_record_value_leak_401, int_key_displaced_plus_survivors_no_double_free_4_8) {
    // Generic-key displaced + surviving: same no-leak / no-double-free contract.
    // JIT exit code reads the surviving key-1 → 7.
    expectValueAndLeakFree(prog(kIntDisplacedPlusSurvivors), 7);
}

BEZEL_TEST(codegen_dict_record_value_leak_401, str_key_explicit_dispose_no_double_free_4_8) {
    // Explicit Dispose() frees the box + table; the scope-end disposal is a
    // §28.2 no-op (slot nulled). JIT exit code = 4 + 5 = 9. LSan leg: no leak,
    // no double-free of the box or table.
    expectValueAndLeakFree(prog(kStrExplicitDispose), 9);
}

// ---- #394 regression guard: OwningDictionary class values still once (§4.9) -

BEZEL_TEST(codegen_owned_dict_class_value_guard_401, class_values_disposed_once_not_box_freed_4_9) {
    // OwningDictionary<string, R> (R a class): the #401 box-free MUST NOT touch
    // these class values — they are disposed exactly once via #394's owning-
    // class value-disposer → R._n == 2. LSan leg: no double-free / no leak.
    expectValueAndLeakFree(kOwnedDictClassValuesGuard, 2);
}
