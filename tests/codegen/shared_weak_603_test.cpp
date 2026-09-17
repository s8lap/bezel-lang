// Codegen tests for Shared<T> / Weak<T> — scoped shared ownership
// (issue #603 / spec §28.13 / stdlib §2.16).
//
// Shared<T> wraps an owned class instance with an atomic reference count.
// Clone() creates an additional strong handle (+1 strong).  Get() borrows the
// inner value.  Downgrade() creates a weak handle (+1 weak, strong unchanged).
// TryUpgrade() promotes a weak handle to a strong one (returns bool).
// The owned value is disposed exactly once when the last strong handle is
// released.  The control block is freed when the last weak handle is released.
//
// Test groups:
//   1. Basic construction / scope-end disposal (inner disposed exactly once).
//   2. Clone: two handles, inner disposed only when both released.
//   3. Get() borrow: field read through borrow.
//   4. Downgrade / TryUpgrade — success path (strong handle still alive).
//   5. No-leak LSan: Shared + Weak with no surviving handle → clean.
//
// Spec refs (v0.134):
//   §28.13 "Shared<T> and Weak<T>".
//   stdlib §2.16 "Shared<T>", "Weak<T>".

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "codegen/LlvmBackend.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile, link against ASan runtime, run under LSan.
// Returns 0 (clean), 1 (sanitizer found a problem), -1 (skip).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_603_lsan_" + std::to_string(counter++);
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

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

void expectLeakFree(const std::string& source, int expectedExit) {
    BEZEL_EXPECT_EQ(run(source), expectedExit);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;    // toolchain unavailable: skip LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 == LSan clean (no leak, no UAF/DF)
}

// A Bezel class with a static dispose-counter so tests can verify the owned
// value is disposed exactly once.
// Syntax: `destructor ... end destructor;` (no `public`, per §12 / grammar).
const char* kCounterClass =
    "module M; "
    "class Inner "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
    "end Inner; ";

} // anonymous namespace


// ──────────────────────────────────────────────────────────────────────────────
// Group 1: basic construction and scope-end disposal.
// §28.13: "The value is disposed exactly once when the last strong handle is
// released."
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 / stdlib §2.16: new Shared<T>(owned obj) consumes obj; while h is alive
// the inner value is NOT disposed (disposed count is still 0).
// The return is inside the method scope where h is still alive → returns 0.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_inner_not_disposed_while_handle_alive) {
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    // h is still alive when return executes → Inner.disposed == 0.
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.13: scope-end releases the last strong handle → inner disposed (count 1).
// Uses a sub-method that creates and releases the handle.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_scope_end_disposes_inner_exactly_once) {
    // Helper method creates a Shared<Inner> handle and returns (scope-end releases it).
    // After the call, Inner.disposed should be 1.
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method CreateAndRelease() "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "  end CreateAndRelease; "
        "  public static method Main(): int "
        "    C.CreateAndRelease(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    expectLeakFree(src, 1);
}


// ──────────────────────────────────────────────────────────────────────────────
// Group 2: Clone — inner disposed only when all strong handles released.
// §28.13: Clone() increments the strong count; each Clone handle must be
// released before the inner value is disposed.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 / stdlib §2.16: h1 (original) + h2 (clone) — two strong handles.
// After h1 and h2 are released, inner is disposed exactly once.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_clone_two_handles_dispose_once) {
    // Both handles released via two helper calls; inner disposed exactly once.
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method TwoHandles() "
        "    var obj := new Inner(); "
        "    using var h1 := new Shared<Inner>(owned obj); "
        "    using var h2 := h1.Clone(); "
        "  end TwoHandles; "
        "  public static method Main(): int "
        "    C.TwoHandles(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    expectLeakFree(src, 1);
}

// §28.13: three handles (original + 2 clones), all released via scope-end →
// inner disposed exactly once (count 1).
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_three_handles_dispose_once) {
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method ThreeHandles() "
        "    var obj := new Inner(); "
        "    using var h1 := new Shared<Inner>(owned obj); "
        "    using var h2 := h1.Clone(); "
        "    using var h3 := h1.Clone(); "
        "  end ThreeHandles; "
        "  public static method Main(): int "
        "    C.ThreeHandles(); "
        "    if Inner.disposed = 1 then return 0; end; "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    expectLeakFree(src, 0);
}


// ──────────────────────────────────────────────────────────────────────────────
// Group 3: Get() borrow.
// §28.13: Get() returns a borrowed reference to the inner value.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13 / stdlib §2.16: Get() returns the inner T pointer; field reads work.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_get_returns_readable_inner) {
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 42; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    var b := h.Get(); "
        "    if b.V = 42 then return 0; end; "
        "    return 1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}


// ──────────────────────────────────────────────────────────────────────────────
// Group 4: Downgrade / TryUpgrade.
// §28.13: Downgrade() creates a Weak<T> handle; TryUpgrade() succeeds while
// at least one strong handle is alive.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13: Downgrade() + TryUpgrade() succeeds while h is alive.
// The new strong handle from TryUpgrade is valid and Get() works on it.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_tryupgrade_succeeds_while_strong_alive) {
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 7; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    using var w := h.Downgrade(); "
        "    if w.TryUpgrade(out using var h2) then "
        "      if h2.Get().V = 7 then return 0; end; "
        "      return 1; "
        "    end; "
        "    return 2; "
        "  end Main; "
        "end C;";
    expectLeakFree(src, 0);
}


// ──────────────────────────────────────────────────────────────────────────────
// Group 5: LSan — no leak for basic Shared + Weak lifetime.
// §28.13: the control block is freed when the last weak handle is released.
// ──────────────────────────────────────────────────────────────────────────────

// §28.13: Shared<T> + Weak<T> both released → inner disposed, ctrl block freed
// → LSan reports no leaks. This is the cycle-break pattern without a real cycle.
BEZEL_TEST(codegen_shared_weak_603, spec_28_13_shared_and_weak_no_leak_lsan) {
    std::string src = std::string(kCounterClass) +
        "class C "
        "  public static method RunAndRelease() "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "    using var w := h.Downgrade(); "
        "  end RunAndRelease; "
        "  public static method Main(): int "
        "    C.RunAndRelease(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    expectLeakFree(src, 1);
}
