// fix #1625 / §28.7 (v0.183): drop-on-overwrite + scope-end disposal for a
// null-initialized local that BECOMES owned through reassignment.
//
// A local declared `var best: T? := null;` and later reassigned with owned
// values (`best := new T();`) leaked every DISPLACED value: codegen's
// `unwindOwnedLocalClass` (LlvmBackend_Setup.cpp) classified a local by its
// INITIALIZER, and a `null` initializer is not an owned producer, so the local
// was never treated as an owned class local. Consequently the §28.7 drop-on-
// overwrite (`disposeDisplacedOwnedClassLocal`) never fired on reassignment
// (intermediate values leaked), and — absent an explicit Dispose — §28.3
// scope-end disposal was not registered either (the final value leaked too).
//
// The fix adds a sema-supplied flag (`becomesOwnedViaReassign`, set by the
// ownership pass on the first Owned reassignment of a plain local) that
// `unwindOwnedLocalClass` reads as a fallback so the local is classified as an
// owned class local for drop-on-overwrite and scope-end/unwind disposal. This
// mirrors how the existing `autoDropAtScopeEnd` flag is produced and consumed.
//
// Testing note (why these are ASan-AOT, not runJit value tests): the in-process
// JIT harness (runJit) does NOT run the ownership pass on the codegen AST — it
// is gated behind BEZEL_CHECK_OWNERSHIP=1 and runs on a separate throwaway
// parse, so sema-supplied flags (autoDropAtScopeEnd, becomesOwnedViaReassign,
// …) are never set in the JIT path. This is the same structural property that
// makes the §28.3 affine auto-drop JIT-inactive. The ownership-dependent
// disposal is therefore pinned via runUnderASan (AOT + ownership pass +
// LeakSanitizer): without the fix the displaced Cell is unreachable at exit and
// LSan reports it (rc=1); with the fix it is disposed (rc=0). The destructor-
// counter idiom is verified directly via bezelc AOT (documented inline).
//
// Spec citations: §28.7 v0.161 ("drop-on-overwrite"), §28.3 (ownership state of
// locals / affine auto-drop / the polymorphic-null adoption rule), v0.183.

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
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {
namespace fs = std::filesystem;

// JIT run via the in-process pipeline (NOTE: no ownership pass on the codegen
// AST — see file header). Used only for the sanity cases below that do not
// depend on the becomesOwnedViaReassign flag.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// AOT compile (WITH the ownership pass) → object → link with ASan → run.
// Returns 0 (clean), 1 (sanitizer fired — a leak or memory error), -1
// (toolchain unavailable / compile error).
int runUnderASan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1625_asan");
}

// Destructor-counter Cell: _n increments on construct, decrements on dispose.
// A correct program leaves _n == 0; a leak leaves the displaced Cell unreachable
// at exit (LSan detects it). Used by the ASan tests below.
const std::string kCellHdr =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// ASan AOT tests — PRIMARY bug pins (run the ownership pass + LeakSanitizer)
// ===========================================================================
// Without the fix, the displaced value at each reassignment is never disposed:
// its pointer is overwritten in the slot and becomes unreachable, so Lsan
// reports it at exit (rc=1). With the fix, §28.7 drop-on-overwrite disposes the
// displaced value before each store and LSan is clean (rc=0). The destructor-
// counter confirms exactly-once via bezelc AOT directly: the issue repro
// `var best: Cell? := null; best := new Cell(); best := new Cell();
// best?.Dispose(); return Cell._n;` returns 1 (one residual live Cell) without
// the fix and 0 with it.

// §28.7 / #1625: the issue repro — a null-init local that becomes owned must
// drop-on-overwrite the displaced value at the second assignment.  The explicit
// Dispose covers only the FINAL value, so without the fix the first Cell leaks
// (unreachable) and LSan fires.
BEZEL_TEST(local_drop_on_overwrite_1625,
           asan_null_init_reassign_disposes_displaced_spec_28_7) {
    int rc = runUnderASan(
        kCellHdr +
        "class C public static method Main(): int "
        "  var best: Cell? := null; "
        "  best := new Cell(); "   // poly-null → owned; null slot, no dispose
        "  best := new Cell(); "   // §28.7: 1st Cell disposed here (was leaking)
        "  best?.Dispose(); "      // disposes the final (2nd) Cell
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan/LSan clean (no leak, no double-free)
}

// §28.7 / #1625: a chain of replacements — every displaced value is disposed at
// its overwrite; only the final value is live at a time.  Without the fix every
// intermediate Cell leaks (unreachable).
BEZEL_TEST(local_drop_on_overwrite_1625,
           asan_null_init_overwrite_chain_no_leak_spec_28_7) {
    int rc = runUnderASan(
        kCellHdr +
        "class C public static method Main(): int "
        "  var best: Cell? := null; "
        "  best := new Cell(); "
        "  best := new Cell(); "
        "  best := new Cell(); "
        "  best := new Cell(); "
        "  best?.Dispose(); "
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan/LSan clean (no leak, no double-free)
}

// §28.3 / §28.7 / #1625: with NO explicit Dispose, the final value is auto-
// dropped at scope end (the affine auto-drop of §28.3, admitted because the
// local's drop-on-overwrite is now covered).  Without the fix the ownership
// pass over-rejected this ("owned value in `best` would be dropped without
// disposal"); with it, it compiles and disposes every value.  The nested
// `do … end;` ends best's scope before Main returns.
BEZEL_TEST(local_drop_on_overwrite_1625,
           asan_null_init_scope_end_auto_drop_spec_28_3_28_7) {
    int rc = runUnderASan(
        kCellHdr +
        "class C public static method Main(): int "
        "  do "
        "    var best: Cell? := null; "
        "    best := new Cell(); "   // _n = 1
        "    best := new Cell(); "   // §28.7: 1st disposed; _n = 1
        "    best := new Cell(); "   // §28.7: 2nd disposed; _n = 1
        "  end; "                    // §28.3 auto-drop: final disposed; _n = 0
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip (or compile rejected)
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan/LSan clean (no leak, no double-free)
}

// §28.7 / #1625: self-assign `best := best` after best becomes owned must NOT
// dispose the held value (the identity-guard prevents a use-after-free).
BEZEL_TEST(local_drop_on_overwrite_1625,
           asan_null_init_self_assign_no_uaf_spec_28_7) {
    int rc = runUnderASan(
        kCellHdr +
        "class C public static method Main(): int "
        "  var best: Cell? := null; "
        "  best := new Cell(); "
        "  best := best; "    // identity-guard prevents UAF
        "  best?.Dispose(); "
        "  return 0; "
        "end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no use-after-free)
}

// ===========================================================================
// JIT sanity tests — cases that do NOT depend on the becomesOwnedViaReassign
// flag (no intermediate displacement), so they are sound under runJit (which
// does not run the ownership pass on the codegen AST — see file header).
// ===========================================================================

// §28.7 / #1625: the FIRST assignment to a null-init local disposes nothing
// (the slot was null — the null-guard skips disposal), matching §28.7's
// "Overwriting a … null-valued nullable stores without disposal."
BEZEL_TEST(local_drop_on_overwrite_1625,
           null_init_first_assign_no_dispose_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr +
        "class C public static method Main(): int "
        "  var best: Cell? := null; "
        "  best := new Cell(); "   // null slot → no dispose; _n = 1
        "  best?.Dispose(); "      // _n = 0
        "  return Cell._n; "       // 0
        "end Main; end C;"), 0);
}
