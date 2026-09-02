// Codegen + runtime — issue #993: an OWNED class temporary in a method-call OR
// constructor-call argument list must be auto-disposed when a LATER argument's
// evaluation throws before the call runs (§28.3 exception-path discharge).
//
// §28.3 (line ~4968, "Exception-path discharge"): owned locals/parameters live
// when an exception propagates out of their enclosing block are disposed
// implicitly during stack unwinding — the compiler treats each as if wrapped in
// an implicit try/finally. The same principle is cited for the `new T(...)`
// result of an object-initializer block (§ line ~3829: "the instance is in an
// unattached-owned state, and if any setter throws, the instance is disposed by
// the implicit try/finally cleanup before the exception propagates") and for the
// owning collection-literal form (§ line ~2166: "If element evaluation throws
// partway, the already-evaluated owned elements ... are disposed by the standard
// exception-path discharge"). A call/constructor argument list is the same shape:
// arguments evaluate left-to-right BEFORE the callee runs, so an already-built
// owned `new T()` temporary in an earlier slot is "unattached-owned" while later
// arguments evaluate.
//
// THE BUG: `buildOrderedArgs` lowered each argument eagerly but registered no
// disposal for an already-evaluated owned temporary. When a LATER argument threw
// (e.g. `C.Use(new Cell(), C.Throw())`), control left the argument list WITHOUT
// invoking the callee, so the constructed `Cell` was orphaned — no owning
// local/field/slot, no disposal entry — and LEAKED. The fix registers each owned
// `new T()` (and `move x` of an owned class) argument temporary as an
// `unwindOnly` disposal in the current scope while the remaining arguments
// evaluate, then DISCHARGES them (truncates the scope) once every argument is
// evaluated and the call is about to consume them.
//
// DISPOSE-EXACTLY-ONCE (no-GC): on the THROW-before-call path the transient
// unwind disposal disposes the orphan exactly once (the leak this fixes). On the
// NORMAL path the entry is `unwindOnly` (skipped on normal exit) AND is discharged
// before the call, so the callee's `owned` parameter takes ownership and disposes
// the temporary exactly once — no double-free. A `move x` source is moved-from
// (its own slot/owned-field disposal is suppressed), so the transient disposal is
// the sole unwind disposer of the moved-out value — also no double-free.
//
// Each Cell counts net-live instances (ctor ++, dtor --); `Main` returns the
// counter, so the exit code is the number of Cells still alive. 0 == every Cell
// disposed exactly once (no leak, no double-free); 1 (the bug) == a Cell leaked.
// The leaked orphan stays pointer-reachable at exit, so LSan does NOT flag it —
// the net-live exit code is the load-bearing leak detector; ASan is the
// double-free / use-after-free detector for the normal/consuming paths.

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
#include "test_harness.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_993_" + std::to_string(counter++);
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

// Re-lower + link against the ASan runtime and run under ASan. Verifies MEMORY
// SAFETY (no use-after-free / double-free); a leak is tolerated (detect_leaks=0)
// — the net-live exit code in compileAndRun is the leak detector. Returns the
// program exit code on a clean run, 99 on an ASan-reported corruption, or -1 when
// the toolchain is unavailable / a build step failed (SKIP).
int runUnderAsan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsanExitCode(source, "bezel_993");
}

// Assert the net-live exit code AND (when the toolchain is present) that ASan
// reports no memory-safety corruption (the run exits with the same code, not 99).
void expectLiveCountAndNoCorruption(const std::string& source, int expected) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expected);
    int asan = runUnderAsan(source);
    if (asan == -1) return;            // toolchain unavailable: skip the ASan leg
    BEZEL_EXPECT_EQ(asan, expected);   // no corruption (would be 99)
}

// --- Programs ------------------------------------------------------------

// §28.3 (HEADLINE #993, method call): `new Cell()` (arg 1) is built, then
// `C.Throw()` (arg 2) throws BEFORE `Use` runs. The orphaned Cell must be
// disposed on unwind. Before the fix it leaked (Cell.n == 1); fixed: Cell.n == 0.
const char* kMethodArgLaterThrows =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Throw(): int throw new AppErr(); end Throw; "
    "  public static method Use(owned c: Cell, x: int) c.Dispose(); end Use; "
    "  public static method Main(): int "
    "    try C.Use(new Cell(), C.Throw()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (#993, constructor call): `new Holder(new Cell(), C.Throw())` — the inner
// `new Cell()` temp is orphaned when `C.Throw()` (a later ctor argument) throws
// before the `Holder` constructor runs. Disposed on unwind: Cell.n == 0.
const char* kCtorArgLaterThrows =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _c: Cell?; "
    "  public constructor(owned c: Cell, x: int) this._c := c; end constructor; "
    "end Holder; "
    "class C "
    "  public static method Throw(): int throw new AppErr(); end Throw; "
    "  public static method Main(): int "
    "    try using var h := new Holder(new Cell(), C.Throw()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (#993, TWO temps): both already-evaluated `new Cell()` temporaries (args
// 1 and 2) are disposed when the third argument throws. Cell.n == 0 (was 2).
const char* kTwoTempsLaterThrows =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Throw(): int throw new AppErr(); end Throw; "
    "  public static method Use2(owned a: Cell, owned b: Cell, x: int) "
    "    a.Dispose(); b.Dispose(); "
    "  end Use2; "
    "  public static method Main(): int "
    "    try C.Use2(new Cell(), new Cell(), C.Throw()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (#993, throw BEFORE any owned temp): the throwing argument is FIRST, so
// no `new Cell()` temporary is constructed before the throw — there is nothing to
// dispose and no spurious disposal. Cell.n == 0 (no Cell ever built).
const char* kThrowBeforeAnyTemp =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Throw(): int throw new AppErr(); end Throw; "
    "  public static method Use(x: int, owned c: Cell) c.Dispose(); end Use; "
    "  public static method Main(): int "
    "    try C.Use(C.Throw(), new Cell()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (#993, moved-from local): the issue notes a `move`d plain-`var` local also
// reproduces. `move tmp` transfers ownership out of `tmp` (its own unwind disposal
// is suppressed); the moved-out value lives only in the argument slot and is
// orphaned when `C.Throw()` (the next argument) throws. Disposed on unwind: 0.
const char* kMovedLocalLaterThrows =
    "module M; "
    "class AppErr: Exception end AppErr; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Throw(): int throw new AppErr(); end Throw; "
    "  public static method Use(owned c: Cell, x: int) c.Dispose(); end Use; "
    "  public static method Main(): int "
    "    try "
    "      var tmp := new Cell(); "
    "      C.Use(move tmp, C.Throw()); "
    "    catch AppErr => return Cell.n; "
    "    end; "
    "    return -1; "
    "  end Main; "
    "end C;";

// §28.3 (#993, NORMAL path, no double-free): every argument evaluates, the call
// runs, and the callee's `owned` parameter disposes the temporary EXACTLY once.
// The transient unwind disposal must be discharged before the call so it does not
// dispose the temporary a second time. Cell.n == 0 AND ASan: no double-free.
const char* kNormalPathDisposeOnce =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Use(owned c: Cell, x: int) c.Dispose(); end Use; "
    "  public static method Main(): int "
    "    C.Use(new Cell(), 5); "
    "    return Cell.n; "
    "  end Main; "
    "end C;";

// §28.3 (#993, NORMAL path, ctor): the constructor consumes the `new Cell()` temp
// into an owned field disposed at the Holder's scope end. Disposed exactly once
// (Cell.n == 0); the discharged transient disposal introduces no double-free.
const char* kNormalCtorDisposeOnce =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned _c: Cell?; "
    "  public constructor(owned c: Cell, x: int) this._c := c; end constructor; "
    "end Holder; "
    "class C "
    "  public static method Build() "
    "    using var h := new Holder(new Cell(), 5); "
    "  end Build; "
    "  public static method Main(): int "
    "    C.Build(); "
    "    return Cell.n; "
    "  end Main; "
    "end C;";

// §28.3 (#993, NORMAL path, moved local): `move tmp` to an owned parameter on the
// normal path — disposed exactly once by the callee; the moved-from `tmp` is not
// disposed again. Cell.n == 0 AND ASan: no double-free.
const char* kNormalMovedLocalDisposeOnce =
    "module M; "
    "class Cell "
    "  public static n: int := 0; "
    "  public constructor() n := n + 1; end constructor; "
    "  destructor n := n - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Use(owned c: Cell, x: int) c.Dispose(); end Use; "
    "  public static method Main(): int "
    "    var tmp := new Cell(); "
    "    C.Use(move tmp, 5); "
    "    return Cell.n; "
    "  end Main; "
    "end C;";

} // namespace

BEZEL_TEST(owned_arg_temp_unwind_993, method_arg_later_throws_disposes_orphan) {
    expectLiveCountAndNoCorruption(kMethodArgLaterThrows, 0);  // was 1 (leaked)
}

BEZEL_TEST(owned_arg_temp_unwind_993, ctor_arg_later_throws_disposes_orphan) {
    expectLiveCountAndNoCorruption(kCtorArgLaterThrows, 0);    // was 1 (leaked)
}

BEZEL_TEST(owned_arg_temp_unwind_993, two_temps_later_throws_both_disposed) {
    expectLiveCountAndNoCorruption(kTwoTempsLaterThrows, 0);   // was 2 (both leaked)
}

BEZEL_TEST(owned_arg_temp_unwind_993, throw_before_any_temp_no_spurious_dispose) {
    expectLiveCountAndNoCorruption(kThrowBeforeAnyTemp, 0);
}

BEZEL_TEST(owned_arg_temp_unwind_993, moved_local_later_throws_disposes_orphan) {
    expectLiveCountAndNoCorruption(kMovedLocalLaterThrows, 0); // was 1 (leaked)
}

BEZEL_TEST(owned_arg_temp_unwind_993, normal_path_disposes_exactly_once) {
    // Discharged transient disposal: the callee disposes once; no double-free.
    expectLiveCountAndNoCorruption(kNormalPathDisposeOnce, 0);
}

BEZEL_TEST(owned_arg_temp_unwind_993, normal_ctor_disposes_exactly_once) {
    expectLiveCountAndNoCorruption(kNormalCtorDisposeOnce, 0);
}

BEZEL_TEST(owned_arg_temp_unwind_993, normal_moved_local_disposes_exactly_once) {
    expectLiveCountAndNoCorruption(kNormalMovedLocalDisposeOnce, 0);
}
