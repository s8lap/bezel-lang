// Codegen + runtime — boxed value-type reference counting (issue #422, boxes
// increment). Per §8.6, a value type used where `Object` (or an interface it
// implements) is expected is BOXED: a heap wrapper holds the value. Per §28.9,
// boxed value types are runtime-managed — exempt from manual disposal, their
// memory reclaimed automatically. Before this change the box wrapper allocated
// by `boxValue` was intentionally leaked (the comment at boxValue even said so).
//
// THE BOX-vs-CLASS HAZARD (the reason this is the highest-risk #422 increment):
// a box `{ vtable*, rc, value-bytes }` and a CLASS INSTANCE `{ vtable*,
// disposed-flag, fields }` share the same static type (`Object`/interface) and
// the same first-word-is-a-vtable shape. Class instances are NOT refcounted
// (they are owned/disposed, with the #411 disposed flag). A box-release at an
// `Object`-typed location must free a box but must NEVER touch a class instance.
//
// THE DISCRIMINATOR (placement-layer, mirroring strings/closures): codegen emits
// a scope-end box release ONLY for a `var` Object/interface local whose
// DECLARATION INITIALIZER has a boxable VALUE static type — i.e. a local that
// codegen ITSELF just boxed via boxValue (a codegen-internal +1 box producer).
// A class instance coerced to Object (the #399 path) has a REFERENCE static type
// at the initializer, so boxForTarget is a no-op there and the local is NEVER
// eligible. The runtime helper is therefore never handed a class instance: the
// box-vs-class distinction is resolved by the static type at the producer site,
// exactly as bz_string_release / bz_closure_release are only emitted for locals
// codegen knows hold a producer's +1 reference. Escapes (returned/stored/passed/
// aliased) are conservatively LEFT leaked (a leak is acceptable; a double-free /
// use-after-free / corrupting the class model is NOT).
//
// Two complementary checks, mirroring string_refcount_test.cpp:
//   1. compileAndRun: native exit code proves correctness (value round-trips).
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir422box() {
    return wtScratchDir("wt-422box");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir422box();
    std::string stem = unique_stem("bezel_422box");
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

    auto dir = wtDir422box();
    std::string stem = unique_stem("bezel_422box_lsan");
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
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
// (use-after-free / double-free), but TOLERATE a leak. Used both for the
// conservatively-leaked escape cases (returned / field-stored / aliased boxes)
// where this increment intentionally over-retains, AND — decisively — for the
// CLASS-INSTANCE-IN-OBJECT path, where the class instance must be disposed via
// ownership exactly once and must NOT be touched by box-release. A double-free /
// UAF aborts non-zero under ASan; a pure leak does not. We run with
// detect_leaks=0 and require the program's own exit code, which still catches
// every memory-safety corruption (the thing that must NOT regress) while
// permitting a deliberate leak.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return;  // toolchain unavailable: skip
    auto dir = wtDir422box();
    std::string stem = unique_stem("bezel_422box_safe");
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
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return; }
    // exitcode=99: a DISTINCT ASan failure code so a UAF/double-free can never
    // be confused with the program legitimately returning expectedExitCode
    // (ASan's default error exitcode is 1, masking a UAF in any test that
    // expects 1). No test expects 99.
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

// §8.6 (load-bearing): a long loop that boxes a fresh value (an int) into a
// non-escaping `Object` local each iteration and reads it via `is int` only
// (a non-consuming read). The local is initialized by a value-typed source (a
// +1 box producer) and never escapes, so its box is released at scope end (each
// iteration) → LSan-clean steady state. Before the fix every iteration leaked
// the box wrapper. Returns 0 (acc counts every iteration's matching `is int`).
const char* kLoopBoxIntSteadyState =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var o: Object := i; "
    "      if o is int then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 200000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6: a loop boxing a fresh RECORD value into a non-escaping `Object` local
// each iteration; read via `as` unbox of the value bytes (non-consuming — `as`
// copies the payload, it never retains the box pointer). The boxed record is
// released each iteration → LSan-clean. "Rect(3,4) as Rect → w=3" accumulates.
const char* kLoopBoxRecordSteadyState =
    "module M; "
    "record Rect(w: int, h: int); "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 100000 do "
    "      var o: Object := new Rect(3, 4); "
    "      var r := o as Rect; "
    "      acc := acc + r!.w; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 300000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6 / §12.7: a boxed value round-trips through `is`/`as` correctly while
// being released — value correctness of the reclaimed-box path. A boxed int
// matched by `is int` and unboxed by `as int` yields 42.
const char* kBoxRoundTrip =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 42; "
    "    if o is int then "
    "      var n: int? := o as int; "
    "      return n!; "                 // 42
    "    end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6 Nullable value-type boxing: a null int? boxes to a NULL Object reference.
// The local holds null; box-release is null-safe (a no-op) → no crash, no leak.
const char* kNullBoxedIsNull =
    "module M; class C "
    "  public static method Main(): int "
    "    var a: int? := null; "
    "    var o: Object? := a; "
    "    if o = null then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// --- The DECISIVE class-instance guard (must NOT be double-freed) --------

// §28.9 / #399: a CLASS INSTANCE coerced to Object then dropped. The initializer
// `new Dog()` has a REFERENCE static type, so boxForTarget is a no-op — NO box,
// the local is NOT a box-release-eligible producer. The instance is owned and
// disposed via ownership (`using var`) exactly once; box-release must NEVER touch
// it. If the discriminator were unsound (box-release firing on a class instance)
// this would double-free / corrupt and ASan would abort. We require no
// corruption and the right exit. THIS IS THE LOAD-BEARING SAFETY TEST.
const char* kClassInstanceInObjectDisposedOnce =
    "module M; "
    "class Animal end Animal; "
    "class Dog : Animal end Dog; "
    "class C "
    "  public static method Main(): int "
    "    using var d: Object := new Dog(); "
    "    return (if d is Animal then 7 else 0); "   // 7
    "  end Main; "
    "end C;";

// §28.9 / #399: a class instance assigned to an interface-typed `using var`,
// dispatched through the interface, then disposed via ownership exactly once.
// Again NO box (reference initializer) → box-release never fires on it. A loop
// to make any double-free/UAF overwhelmingly likely to be caught.
const char* kClassInterfaceLoopDisposedOnce =
    "module M; "
    "interface IShape method Area(): int; end IShape; "
    "class Square : IShape public method Area(): int => 9; end Square; "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 50000 do "
    "      using var s: IShape := new Square(); "
    "      acc := acc + s.Area(); "
    "      i := i + 1; "
    "    end; "
    "    if acc = 450000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §8.6 / §28.9: the adversarial MIX — a boxed VALUE local (released each
// iteration) and a CLASS-instance Object local (disposed via ownership each
// iteration) live in the SAME loop scope. The box must be freed by box-release;
// the class instance must be disposed via ownership exactly once and never
// touched by box-release. 50k iterations make any cross-talk (double-free / UAF
// / freeing the wrong cell) overwhelmingly likely to be caught by ASan, AND the
// boxed-value side must be LSan-clean (it is non-escaping and released). So this
// runs under the FULL LSan leg (no leak AND no corruption). The decisive
// combined guard.
const char* kBoxAndClassMixedLoop =
    "module M; "
    "class Animal end Animal; "
    "class Dog : Animal end Dog; "
    "record Rect(w: int, h: int); "
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 50000 do "
    "      var o: Object := new Rect(3, 4); "       // BOX → released each iter
    "      using var d: Object := new Dog(); "      // CLASS → owned/disposed
    "      var r := o as Rect; "
    "      if d is Animal then acc := acc + r!.w; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 150000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// --- Conservatively-leaked escapes (value + no-corruption) ---------------

// §28.9: a boxed value escapes by being bound into ANOTHER local (alias). The
// declaration local `o` escapes (bound into `b`), so it is NOT released; `b`'s
// initializer is an identifier (not a +1 box producer) so it is NOT released
// either — the box is leaked exactly once, never double-freed. The DECISIVE
// box double-free guard: sharing a box between two Object names must never free
// it twice. Correctness: both names observe the same boxed int.
const char* kBoxAliasShared =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 5; "
    "    var b: Object := o; "
    "    var x: int? := o as int; "
    "    var y: int? := b as int; "
    "    if x! = y! then return x!; end; "   // 5
    "    return 0; "
    "  end Main; "
    "end C;";

// §28.9: a boxed value escapes by being passed as a call argument. The local
// `o` escapes (the argument position), so it is conservatively leaked, never
// released by the caller (no UAF in the callee). Returns the unboxed value.
const char* kBoxPassedAsArg =
    "module M; class C "
    "  public static method Unbox(o: Object): int "
    "    var n: int? := o as int; "
    "    return n!; "
    "  end Unbox; "
    "  public static method Main(): int "
    "    var o: Object := 9; "
    "    return C.Unbox(o); "             // 9
    "  end Main; "
    "end C;";

// --- FINDING [1] (§28.9 / §28.3): a +1 box wrapper captured by a lambda THROUGH
// a safe-read sub-position (an `is`/`as` operand) must NOT be released at scope
// end. The closure's heap env borrows (aliases) the captured box and outlives
// the producing scope (§28.9: boxed values are runtime-managed; §28.3
// classifies a capture as a borrow). Before the fix the escape walker re-applied
// the `is`/`as`-operand safe-read suppression inside the lambda body, judged `o`
// non-escaping, and armed a scope-end bz_box_release — a use-after-free (ASan:
// heap-buffer-overflow in bz_box_release on the freed env cell). The fix makes
// ANY captured-local read inside a lambda an escape, so `o` is conservatively
// LEFT leaked (leak-over-UAF). Value + no-corruption.

// FINDING [1] E1: captured box read as an `is` operand inside the lambda.
// NB: the return value must NOT be 1 — ASan's default error exit code is 1, so a
// UAF-abort would be indistinguishable from a clean exit 1 under the
// no-corruption harness (which does not override exitcode). 7 disambiguates.
const char* kCaptureBoxIsOperand =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 5; "
    "    var f := lambda() => (o is int); "
    "    var b := f(); "
    "    if b then return 7; end; "    // o is int => true => 7
    "    return 0; "
    "  end Main; "
    "end C;";

// FINDING [1] E1 CONTROL: a bare-identifier box capture `() => o` — already
// classified as an escape before the fix; documents the already-correct case.
// Returns 7 for the same ASan-exit-code disambiguation reason as above.
const char* kCaptureBoxBareIdent =
    "module M; class C "
    "  public static method Main(): int "
    "    var o: Object := 5; "
    "    var f := lambda() => o; "          // outer lambda forwards o
    "    var got: Object := f(); "
    "    if got is int then return 7; end; "  // 7
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// --- LSan-clean tests (the reclaim: leaked before, clean after) ----------

BEZEL_TEST(codegen_box_refcount_422, loop_box_int_steady_state_reclaimed_8_6) {
    // The load-bearing leak test: 200k iterations each freeing the box wrapper
    // at scope end → LSan-clean steady state. Leaked before the fix.
    expectValueAndLeakFree(kLoopBoxIntSteadyState, 0);
}

BEZEL_TEST(codegen_box_refcount_422, loop_box_record_steady_state_reclaimed_8_6) {
    expectValueAndLeakFree(kLoopBoxRecordSteadyState, 0);
}

BEZEL_TEST(codegen_box_refcount_422, box_round_trip_value_and_clean_8_6) {
    expectValueAndLeakFree(kBoxRoundTrip, 42);
}

BEZEL_TEST(codegen_box_refcount_422, null_boxed_is_null_no_leak_8_6) {
    // A null int? boxes to a null Object; box-release is null-safe → no leak.
    expectValueAndLeakFree(kNullBoxedIsNull, 0);
}

// --- The decisive class-instance safety guards (no double-free / UAF) -----

BEZEL_TEST(codegen_box_refcount_422, class_instance_in_object_disposed_once_28_9) {
    // THE load-bearing safety test: a class instance coerced to Object is NOT a
    // box and must be disposed via ownership exactly once — box-release must
    // never touch it. ASan aborts on a double-free; we require a clean exit.
    expectValueAndNoCorruption(kClassInstanceInObjectDisposedOnce, 7);
}

BEZEL_TEST(codegen_box_refcount_422, class_interface_loop_disposed_once_28_9) {
    expectValueAndNoCorruption(kClassInterfaceLoopDisposedOnce, 0);
}

BEZEL_TEST(codegen_box_refcount_422, box_and_class_mixed_loop_clean_8_6) {
    // The decisive combined guard: a boxed value (released) and a class instance
    // (disposed via ownership) in the same loop scope, 50k iterations. Both are
    // freed exactly once and nothing crosses over — LSan-clean AND no corruption.
    expectValueAndLeakFree(kBoxAndClassMixedLoop, 0);
}

// --- Value + no-corruption tests (conservatively-leaked escapes) ---------

BEZEL_TEST(codegen_box_refcount_422, box_alias_shared_freed_once_no_double_free_28_9) {
    expectValueAndNoCorruption(kBoxAliasShared, 5);
}

BEZEL_TEST(codegen_box_refcount_422, box_passed_as_arg_no_uaf_28_9) {
    expectValueAndNoCorruption(kBoxPassedAsArg, 9);
}

// --- FINDING [1] (§28.9 / §28.3): captured-box UAF — was a heap-buffer-overflow
// in bz_box_release before the fix; now a tolerated leak (value + no-corruption).
// The cardinal assertion is NO memory-safety error under ASan.

BEZEL_TEST(codegen_box_refcount_422, capture_box_is_operand_no_uaf_28_9) {
    // E1: () => (o is int) — a captured box read in an `is`-operand safe-read
    // position inside the lambda. UAF in bz_box_release before fix; clean
    // (leaked) after — the env borrows the captured box, so it must not be freed.
    expectValueAndNoCorruption(kCaptureBoxIsOperand, 7);
}

BEZEL_TEST(codegen_box_refcount_422, capture_box_bare_ident_control_no_uaf_28_9) {
    // E1 CONTROL: () => o — a bare-identifier box capture, already an escape
    // before the fix; documents the already-correct case.
    expectValueAndNoCorruption(kCaptureBoxBareIdent, 7);
}
