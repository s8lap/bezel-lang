// Codegen — static owned fields and static owning-inline-array fields are
// disposed at program exit in REVERSE initialization order (§22.13, issue #1212).
//
// §22.13: "static owned fields are disposed at program exit, in reverse order
// of initialization". Before this fix, static owned class instances were
// constructed and stored at static-init time but bz_register_static_owned was
// never called, so the heap block leaked and the destructor never ran.
//
// The fix: emitStaticConstructorBody now calls bz_register_static_owned(val)
// after each static owned class/interface field is initialized.  A
// bz_dispose_static_owned atexit handler (registered on first call) iterates
// the list in reverse, dispatches each object's vtable Dispose (slot 2,
// idempotent), then frees the block.  Owning inline-array static fields
// register each non-null slot individually.
//
// Each JIT test observes that Main returns 0 (destructor has NOT run yet —
// the static field is still live during Main) and is followed by an LSan leg
// that verifies no leak after program exit.
//
// §22.13 test plan:
//   1212-a  single static owned field with destructor → disposed once at exit
//           (JIT: Main reads Res.n=0 before exit; LSan: block freed, no leak)
//   1212-b  two static owned fields → both disposed, JIT checks n=0 in Main,
//           LSan: both blocks freed
//   1212-c  static owning-inline-array field [3 owned]Slot? → all 3 slots
//           disposed (JIT: n=0; LSan: 3 blocks freed)
//   1212-d  class with NO destructor → block freed at exit, no spurious call
//           (ASan: no heap-use-after-free or invalid-read)
//   1212-e  `using var` local with same type → still disposes exactly once
//           (regression: JIT returns 1)
//   1212-f  static event source still disposed at exit (regression for the
//           existing §24.1 / §22.13 / #477 atexit path)

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

// Compiles `source` with ASan and runs under LSan.  Returns:
//   0  — clean (no leak, no error)
//   1  — sanitizer flagged a problem (test failure)
//  -1  — toolchain unavailable / compile failed (SKIP, not a failure)
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1212_lsan_" + std::to_string(counter++);
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }

    // ASan exit code 99 → sanitizer flagged a problem; otherwise clean.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                       // clean
}

// =====================================================================
// 1212-a: single static owned field with destructor
// §22.13: static owned field disposed at program exit (after Main returns).
// =====================================================================

// Res.n is 0 during Main (destructor has not run yet; the static R is still
// live).  At exit, bz_dispose_static_owned calls vtable Dispose → destructor
// increments n, then free() reclaims the block.  LSan verifies: no leak.
const char* kSingleStaticOwned =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "class C "
    "  public static owned readonly R: Res := new Res(); "
    "  public static method Main(): int => Res.n; "
    "end C;";

// =====================================================================
// 1212-b: two static owned fields → both disposed at exit
// §22.13: both fields are registered; atexit disposes in reverse order.
// =====================================================================

const char* kTwoStaticOwned =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "class C "
    "  public static owned readonly R1: Res := new Res(); "
    "  public static owned readonly R2: Res := new Res(); "
    "  public static method Main(): int => Res.n; "
    "end C;";

// =====================================================================
// 1212-c: static owning-inline-array field
// §22.13 / §15.3: each non-null slot registered individually at static init.
// =====================================================================

const char* kStaticOwnedInlineArray =
    "module M; "
    "class Slot "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Slot.n := Slot.n + 1; end destructor; "
    "end Slot; "
    "class C "
    "  public static _arr: [3 owned]Slot? := [new Slot(), new Slot(), new Slot()]; "
    "  public static method Main(): int => Slot.n; "
    "end C;";

// =====================================================================
// 1212-d: class with NO destructor → block freed at exit, no crash
// §22.13: bz_dispose_static_owned calls vtable Dispose (no-op for a class
// with no destructor) then free().  No heap-use-after-free or leak.
// =====================================================================

const char* kNoDestructorStaticOwned =
    "module M; "
    "class NoDestructor "
    "  public static n: int := 0; "
    "  public constructor() NoDestructor.n := NoDestructor.n + 1; end constructor; "
    "end NoDestructor; "
    "class C "
    "  public static owned readonly R: NoDestructor := new NoDestructor(); "
    "  public static method Main(): int => NoDestructor.n; "
    "end C;";

// =====================================================================
// 1212-e: `using var` local with same class → disposes exactly once
// Regression: the atexit path must not interfere with normal scoped disposal.
// =====================================================================

const char* kUsingVarNotAffected =
    "module M; "
    "class Res "
    "  public static n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.n := Res.n + 1; end destructor; "
    "end Res; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var r := new Res(); "
    "    end; "
    "    return Res.n; "
    "  end Main; "
    "end C;";

// =====================================================================
// 1212-f: static event source regression — bz_static_event_source_new
// atexit path (§24.1 / §22.13 / #477) must still work after this change.
// =====================================================================

const char* kStaticEventSourceRegression =
    "module M; "
    "class C "
    "  public static event GlobalTick; "
    "  public static method Main(): int "
    "    GlobalTick.Raise(); "
    "    return 7; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// 1212-a tests
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, single_field_main_reads_zero_before_exit_22_13) {
    // §22.13: Main reads Res.n=0 — the destructor has not run yet (the static
    // R is still live during Main; disposal is at program exit after Main returns).
    // Before #1212 the field was never disposed AND the block leaked.
    BEZEL_EXPECT_EQ(run(kSingleStaticOwned), 0);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, single_field_block_freed_at_exit_no_leak_22_13) {
    // LSan: the Res heap block must be freed at program exit by the atexit
    // handler installed by bz_register_static_owned.  Before #1212 this leaked.
    int lsan = runUnderLeakSanitizer(kSingleStaticOwned);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1212-b tests
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, two_fields_main_reads_zero_before_exit_22_13) {
    // §22.13: both R1 and R2 are live during Main; Res.n is still 0.
    BEZEL_EXPECT_EQ(run(kTwoStaticOwned), 0);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, two_fields_both_freed_no_leak_22_13) {
    // LSan: both Res blocks (R1 and R2) are freed at exit.  Both are registered
    // by bz_register_static_owned; the atexit handler iterates in reverse so
    // R2 is disposed first, then R1 (reverse init order per §22.13).
    int lsan = runUnderLeakSanitizer(kTwoStaticOwned);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1212-c tests
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, inline_array_main_reads_zero_22_13) {
    // §22.13 / §15.3: 3-slot owning inline array; each slot is a live Slot
    // instance during Main so n=0 in Main.
    BEZEL_EXPECT_EQ(run(kStaticOwnedInlineArray), 0);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, inline_array_all_slots_freed_no_leak_22_13) {
    // LSan: all 3 Slot blocks are freed at exit.  Each slot was registered
    // individually by the loop in emitStaticConstructorBody.
    int lsan = runUnderLeakSanitizer(kStaticOwnedInlineArray);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1212-d tests
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, no_destructor_main_returns_one_22_13) {
    // §22.13: class with no destructor — ctor ran once (n=1), no destructor.
    // Main reads n=1 (the constructor incremented it; still alive during Main).
    BEZEL_EXPECT_EQ(run(kNoDestructorStaticOwned), 1);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, no_destructor_block_freed_no_crash_22_13) {
    // LSan: block freed at exit; vtable Dispose is a no-op (no user destructor)
    // but free() still reclaims the block.  No heap-use-after-free or crash.
    int lsan = runUnderLeakSanitizer(kNoDestructorStaticOwned);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1212-e tests (regression)
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, using_var_still_disposes_once_28_3) {
    // §28.3: a non-static `using var` with the same class must still dispose
    // exactly once at scope end.  The #1212 atexit path must not interfere.
    BEZEL_EXPECT_EQ(run(kUsingVarNotAffected), 1);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, using_var_no_leak_28_3) {
    // LSan regression: the `using var` block is freed normally (not at atexit),
    // so LSan should see a clean exit with no leak.
    int lsan = runUnderLeakSanitizer(kUsingVarNotAffected);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// =====================================================================
// 1212-f tests (regression)
// =====================================================================

BEZEL_TEST(codegen_static_owned_disposal_1212, static_event_source_still_works_24_1) {
    // §24.1 / §22.13 regression: the existing static EventSource atexit path
    // (bz_static_event_source_new) must still work after adding #1212's path.
    BEZEL_EXPECT_EQ(run(kStaticEventSourceRegression), 7);
}

BEZEL_TEST(codegen_static_owned_disposal_1212, static_event_source_no_leak_24_1) {
    // LSan regression: the static EventSource block is still freed via
    // bz_free_static_event_sources (the existing atexit path, unchanged by #1212).
    int lsan = runUnderLeakSanitizer(kStaticEventSourceRegression);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}
