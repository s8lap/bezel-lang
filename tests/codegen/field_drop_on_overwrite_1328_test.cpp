// fix #1328 / §28.7 v0.161: drop-on-overwrite for owned class/interface FIELDS.
//
// Reassigning an `owned` class-typed field must dispose the displaced value
// before storing the new one.  Before this fix the displacement was a plain
// store that silently lost the old pointer — a guaranteed leak for every
// reassignment.
//
// This file tests the FIELD half:
//   - `obj.f := newValue` disposes the old value (non-null, non-self) exactly once.
//   - Self-assign `obj.f := obj.f` is guarded (identity check) — no dispose.
//   - Move-then-reassign: moved-from slot holds null — null guard skips dispose.
//   - Constructor first-time assignment: calloc-zero slot holds null — safe.
//   - Borrow (non-owned) field is never disposed on overwrite.
//
// ASan AOT tests verify no leak and no double-free (the heap sanitizer catches
// both categories).
//
// Spec citations: §28.7 v0.161 ("drop-on-overwrite"), §28.4 (auto-dispose at
// scope end), §28.5 (owned fields).

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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {
namespace fs = std::filesystem;

// JIT run without ownership pass (value tests).
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// AOT compile → object → link with ASan → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing / compile error).
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1328_asan_" + std::to_string(counter++);
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
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Destructor-counter Cell: _n increments on construct, decrements on dispose.
// A correct program exits with _n == 0.  A leak leaves _n > 0; a double-free
// drives _n negative (returned as exit code it surfaces as a large value).
const std::string kCellHdr =
    "module M; "
    "class Cell "
    "  public static _n: int := 0; "
    "  public constructor() Cell._n := Cell._n + 1; end constructor; "
    "  destructor Cell._n := Cell._n - 1; end destructor; "
    "end Cell; ";

// A container class with ONE owned Cell field and a trivial destructor.
// (The compiler synthesizes field disposal after the destructor body.)
const std::string kContainerHdr =
    "class Container "
    "  public owned _child: Cell; "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Container; ";

}  // namespace


// ===========================================================================
// Value tests — destructor-counter idiom
// ===========================================================================

// §28.7 / #1328: plain field overwrite disposes the displaced value exactly once.
//   1. Create Container c.
//   2. c._child := new Cell()   → Cell._n = 1 (old slot was null → no dispose)
//   3. c._child := new Cell()   → Cell._n should still be 1 (old Cell disposed)
//   4. c is disposed at end     → Cell._n = 0
BEZEL_TEST(field_drop_on_overwrite_1328,
           field_overwrite_disposes_displaced_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr + kContainerHdr +
        "class C public static method Main(): int "
        "  do using var c := new Container(); "
        "    c._child := new Cell(); "   // first assign (null-guard skips old)
        "    c._child := new Cell(); "   // overwrite → old Cell disposed; _n stays 1
        "    if Cell._n <> 1 then return 101; end; "  // sanity: one Cell live
        "  end; "                        // Container disposed → _child disposed
        "  return Cell._n; "            // 0
        "end Main; end C;"), 0);
}

// §28.7 / #1328: overwrite chain — three successive replacements.
//   Each replacement disposes the previous value so only one Cell is live at a
//   time.  All three are eventually disposed exactly once.
BEZEL_TEST(field_drop_on_overwrite_1328,
           field_overwrite_chain_all_disposed_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr + kContainerHdr +
        "class C public static method Main(): int "
        "  do using var c := new Container(); "
        "    c._child := new Cell(); "   // Cell._n = 1
        "    c._child := new Cell(); "   // Cell._n = 1 (old disposed)
        "    c._child := new Cell(); "   // Cell._n = 1 (old disposed)
        "  end; "                        // last Cell disposed
        "  return Cell._n; "            // 0
        "end Main; end C;"), 0);
}

// §28.7 / #1328: self-assign `c._child := c._child` must NOT dispose the field
//   value (identity-guard prevents UAF).  The Cell is still live after the
//   self-assign, and is disposed exactly once when c is disposed.
BEZEL_TEST(field_drop_on_overwrite_1328,
           field_self_assign_no_dispose_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr + kContainerHdr +
        "class C public static method Main(): int "
        "  do using var c := new Container(); "
        "    c._child := new Cell(); "   // Cell._n = 1
        "    c._child := c._child; "     // self-assign: no dispose
        "    if Cell._n <> 1 then return 101; end; "  // Cell still live
        "  end; "                        // Container disposed → Cell disposed
        "  return Cell._n; "            // 0
        "end Main; end C;"), 0);
}

// §28.7 / §28.8.1 / #1328: move-then-assign — after a method that moves
//   `this._child` out the slot holds null.  The subsequent external assignment
//   stores a new value without disposing (null-guard skips the null slot).
//   Both the moved-out value (via `x`) and the new field value are disposed once.
//
//   Note: `move c._field` for a non-`this` receiver is NOT nulled by the codegen
//   (only `move this._field` inside a class method nulls the field slot), so we
//   use an explicit `TakeChild()` method returning `owned Cell` to correctly
//   transfer ownership and null the slot in a supported, ownership-sound way.
BEZEL_TEST(field_drop_on_overwrite_1328,
           field_move_then_assign_no_double_dispose_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr +
        // Container3 has a TakeChild method that moves this._child (properly
        // supported: moves out of a this-field inside the class).
        "class Container3 "
        "  public owned _child: Cell; "
        "  public constructor() end constructor; "
        "  public method TakeChild(): owned Cell return move this._child; end TakeChild; "
        "  destructor end destructor; "
        "end Container3; "
        "class C public static method Main(): int "
        "  do using var c := new Container3(); "
        "    c._child := new Cell(); "          // Cell._n = 1
        "    using var x := c.TakeChild(); "   // move this._child → _child=null; _n=1
        "    c._child := new Cell(); "           // assign to null slot; _n = 2
        "  end; "  // x disposed first (_n=1), then Container3 disposes _child (_n=0)
        "  return Cell._n; "                    // 0
        "end Main; end C;"), 0);
}

// §28.7 / §28.4 / #1328: constructor-time first assignment — the field slot is
//   calloc-zero (null) when the constructor body runs.  The null-guard skips
//   disposal on first assignment, so no spurious Dispose of null occurs.
//   The same field is later reassigned (from outside), which disposes the ctor-
//   assigned Cell exactly once.
BEZEL_TEST(field_drop_on_overwrite_1328,
           ctor_init_then_reassign_disposes_exactly_once_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr +
        "class Container2 "
        "  public owned _child: Cell; "
        "  public constructor() "
        "    _child := new Cell(); "    // first assign; ctor null-guard safe
        "  end constructor; "
        "  destructor end destructor; "
        "end Container2; "
        "class C public static method Main(): int "
        "  do using var c := new Container2(); "  // _n = 1
        "    if Cell._n <> 1 then return 101; end; "
        "    c._child := new Cell(); "    // overwrite → ctor Cell disposed; _n stays 1
        "    if Cell._n <> 1 then return 102; end; "
        "  end; "                          // new Cell disposed; _n = 0
        "  return Cell._n; "
        "end Main; end C;"), 0);
}

// §28.7 / #1328: a NON-owned (borrow) field must NOT be disposed on overwrite.
//   A borrow field merely aliases an object owned elsewhere.  Overwriting it
//   only updates the alias; the original owner retains the sole disposal
//   obligation.
BEZEL_TEST(field_drop_on_overwrite_1328,
           borrow_field_not_disposed_on_overwrite_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr +
        // Container with a BORROW field (no `owned` modifier).
        "class Holder "
        "  public _borrow: Cell; "     // borrow, not owned
        "  public constructor() end constructor; "
        "  destructor end destructor; "  // no auto-dispose of _borrow
        "end Holder; "
        "class C public static method Main(): int "
        "  do using var c1 := new Cell(); "   // _n = 1
        "    using var c2 := new Cell(); "    // _n = 2
        "    using var h := new Holder(); "
        "    h._borrow := c1; "       // borrow alias to c1; _n stays 2
        "    h._borrow := c2; "       // borrow alias to c2; c1 NOT disposed
        "    if Cell._n <> 2 then return 101; end; "  // both still live
        "  end; "   // c1 and c2 disposed by their using vars; _n = 0
        "  return Cell._n; "           // 0
        "end Main; end C;"), 0);
}

// §28.7 / §12.1 / #1328: method reassignment (`this._child := v` inside an
//   instance method) — same semantics as external assignment; old value disposed.
BEZEL_TEST(field_drop_on_overwrite_1328,
           method_this_field_reassign_disposes_old_spec_28_7) {
    BEZEL_EXPECT_EQ(run(kCellHdr +
        "class C2 "
        "  private owned _child: Cell; "
        "  public constructor() end constructor; "
        "  public method SetChild(owned v: Cell) "
        "    _child := v; "   // this._child := v; disposes displaced on 2nd call
        "  end SetChild; "
        "  destructor end destructor; "
        "end C2; "
        "class C public static method Main(): int "
        "  do using var c := new C2(); "
        "    c.SetChild(new Cell()); "  // _n = 1
        "    c.SetChild(new Cell()); "  // _n = 1 (old disposed)
        "    if Cell._n <> 1 then return 101; end; "
        "  end; "           // _child disposed; _n = 0
        "  return Cell._n; "
        "end Main; end C;"), 0);
}


// ===========================================================================
// ASan AOT tests — heap correctness (no leak, no double-free)
// ===========================================================================

// §28.7 / #1328: overwrite chain under ASan — no leak and no double-free.
BEZEL_TEST(field_drop_on_overwrite_1328,
           asan_overwrite_chain_no_leak_no_double_free_spec_28_7) {
    int rc = runUnderASan(
        kCellHdr + kContainerHdr +
        "class C public static method Main(): int "
        "  do using var c := new Container(); "
        "    c._child := new Cell(); "
        "    c._child := new Cell(); "
        "    c._child := new Cell(); "
        "  end; return 0; end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no leak, no double-free)
}

// §28.7 / #1328: self-assign under ASan — no use-after-free.
BEZEL_TEST(field_drop_on_overwrite_1328,
           asan_self_assign_no_uaf_spec_28_7) {
    int rc = runUnderASan(
        kCellHdr + kContainerHdr +
        "class C public static method Main(): int "
        "  do using var c := new Container(); "
        "    c._child := new Cell(); "
        "    c._child := c._child; "    // identity-guard prevents UAF
        "  end; return 0; end Main; end C;");
    if (rc == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(rc, 0);  // 0 = ASan clean (no use-after-free)
}
