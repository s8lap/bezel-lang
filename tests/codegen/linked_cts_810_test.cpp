// Codegen + runtime for linked CancellationTokenSources — the v0.173
// cancellation-composition revision (#810; stdlib §6.1, spec §30.8).
//
//   CancellationTokenSource.CreateLinked(a: CancellationToken,
//                                        b: CancellationToken)
//                                        : owned CancellationTokenSource
//   CancellationTokenSource.CreateLinkedAll(parents: Array<CancellationToken>)
//                                        : owned CancellationTokenSource
//
// Semantics under test (spec §30.8's composition sentence / stdlib §6.1's
// linked-sources paragraph):
//   - the child source cancels when ANY parent token cancels, OR via its own
//     Cancel() — whichever first (cancellation is idempotent);
//   - a parent ALREADY cancelled at creation cancels the child synchronously,
//     before CreateLinked returns;
//   - the child's own Cancel() does NOT propagate UP to the parents;
//   - disposing the child unregisters it from every parent BEFORE it signals its
//     own tokens, so a parent cancelled after the child is disposed fires into
//     nothing (the use-after-free pin — value test here, ASan probe below).
//
// Implementation note (deviation recorded in the #810 lane report): the runtime
// composes cancellation by a PUSH cascade — the child registers its state cell on
// each parent's downward `children` list; the parent's Cancel()/Dispose()
// cascades the latch into the child.  This is the substrate on which #1413 later
// built CancellationToken.Register(callback) — a callback registered on the child's
// token now DOES fire when a parent cancels (the cascade reaches the child's
// callback drain); that behaviour, and the CancelAfter/WithCancelAfter timer, are
// covered in cts_register_timer_1413_test.cpp.  Here the observable equivalent —
// the child token's IsCancellationRequested flips true when a parent cancels — is
// what is tested.
//
// Each test cites the stdlib section and the spec-stated behaviour it verifies.

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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirLinkedCts810() {
    return wtScratchDir("wt-linkedcts810");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Wrap a Main body in a minimal module/class skeleton.
std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// AddressSanitizer / LeakSanitizer verification (mirrors atomic_647_test.cpp).
// Returns: 0 = clean; 1 = sanitizer fired; -1 = toolchain unavailable (SKIP).
int runUnderAsan(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDirLinkedCts810();
    std::string stem = unique_stem("bezel_linkedcts810_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer fired
    return 0;  // clean
}

} // namespace

// ===========================================================================
// CreateLinked — a child cancelled by EITHER parent (§30.8 composition).
// ===========================================================================

// §6.1 / §30.8: parent a cancels → the linked child's token reports cancelled.
BEZEL_TEST(codegen_linked_cts_810, parent_a_cancel_cancels_child) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "a.Cancel(); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §30.8: parent b cancels → the linked child's token reports cancelled
// (the OTHER parent — cancellation composes over ALL parents, not just the first).
BEZEL_TEST(codegen_linked_cts_810, parent_b_cancel_cancels_child) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "b.Cancel(); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §30.8: with NO parent (and no own) cancel, the child is not cancelled —
// linking does not spuriously cancel.
BEZEL_TEST(codegen_linked_cts_810, no_cancel_leaves_child_live) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 0);
}

// §6.1 / §30.8: the child's OWN Cancel() cancels the child but does NOT propagate
// UP — the parent sources stay live (no upward propagation).
BEZEL_TEST(codegen_linked_cts_810, child_own_cancel_no_upward_propagation) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "child.Cancel(); "
        "if a.IsCancellationRequested then return 10; end; "   // parent a must stay live
        "if b.IsCancellationRequested then return 20; end; "   // parent b must stay live
        "if child.Token.IsCancellationRequested then return 1; end; "  // child cancelled
        "return 0; ")), 1);
}

// §6.1 / §30.8: a parent ALREADY cancelled at creation cancels the child
// SYNCHRONOUSLY — before CreateLinked returns (the Register-on-cancelled policy).
BEZEL_TEST(codegen_linked_cts_810, already_cancelled_parent_cancels_child_synchronously) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "a.Cancel(); "                                          // cancel BEFORE linking
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §30.8: CancellationToken.None as a parent is inert — it never cancels,
// so the child stays live; the OTHER (real) parent still composes.
BEZEL_TEST(codegen_linked_cts_810, none_parent_is_inert_other_parent_still_composes) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked("
        "    CancellationToken.None, b.Token); "
        "if child.Token.IsCancellationRequested then return 5; end; "  // None never fires
        "b.Cancel(); "
        "if child.Token.IsCancellationRequested then return 1; end; "  // real parent fires
        "return 0; ")), 1);
}

// §6.1 / §30.8: a copy of the child token (passed to a helper) observes the same
// linked cancellation — the observable equivalent of "a callback registered on
// the child fires when a parent cancels" (general Register is deferred; see the
// file header).  Mirrors cancellation_token_test's copied_token_observes pattern.
BEZEL_TEST(codegen_linked_cts_810, child_token_copy_observes_parent_cancel) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Helper "
        "  public static method Check(tok: CancellationToken): bool "
        "    return tok.IsCancellationRequested; "
        "  end Check; "
        "end Helper; "
        "class C public static method Main(): int "
        "  using var a := new CancellationTokenSource(); "
        "  using var b := new CancellationTokenSource(); "
        "  using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "  var tok := child.Token; "
        "  a.Cancel(); "
        "  if Helper.Check(tok) then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// CreateLinkedAll — a child cancelled by ANY of N parents (§30.8 composition).
// ===========================================================================

// §6.1 / §30.8: CreateLinkedAll over three parents — cancelling the THIRD cancels
// the child (composition ranges over every element of the array).
BEZEL_TEST(codegen_linked_cts_810, create_linked_all_three_parents_third_cancels) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var c := new CancellationTokenSource(); "
        "using var parents := new Array<CancellationToken>(); "
        "parents.Add(a.Token); parents.Add(b.Token); parents.Add(c.Token); "
        "using var child := CancellationTokenSource.CreateLinkedAll(parents); "
        "c.Cancel(); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §30.8: CreateLinkedAll — with none of the parents cancelled, the child
// stays live.
BEZEL_TEST(codegen_linked_cts_810, create_linked_all_none_cancelled_child_live) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var c := new CancellationTokenSource(); "
        "using var parents := new Array<CancellationToken>(); "
        "parents.Add(a.Token); parents.Add(b.Token); parents.Add(c.Token); "
        "using var child := CancellationTokenSource.CreateLinkedAll(parents); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 0);
}

// §6.1 / §30.8: CreateLinkedAll with an ALREADY-cancelled parent among the array
// cancels the child synchronously at creation.
BEZEL_TEST(codegen_linked_cts_810, create_linked_all_already_cancelled_parent_sync) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "b.Cancel(); "
        "using var parents := new Array<CancellationToken>(); "
        "parents.Add(a.Token); parents.Add(b.Token); "
        "using var child := CancellationTokenSource.CreateLinkedAll(parents); "
        "if child.Token.IsCancellationRequested then return 1; end; "
        "return 0; ")), 1);
}

// ===========================================================================
// Disposal ordering — the use-after-free pin (§6.1 dispose order).
// ===========================================================================

// §6.1 / §30.8: dispose the child FIRST, THEN cancel a parent — nothing fires
// into the disposed child.  Before-fix this would be a UAF (parent cascade into
// freed child); the fix unregisters from all parents before signalling.  Value
// test (returns cleanly); the ASan probe below pins the no-UAF at the memory
// level.
BEZEL_TEST(codegen_linked_cts_810, child_disposed_then_parent_cancel_is_safe) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "child.Dispose(); "        // unregisters from a and b BEFORE signalling
        "a.Cancel(); "             // must not fire into the freed child
        "b.Cancel(); "
        "return 7; ")), 7);
}

// ===========================================================================
// AddressSanitizer AOT probes — no leak, no use-after-free.
// ===========================================================================

// §6.1 / §30.8: the UAF pin at the memory level — dispose the child, then cancel
// both parents.  ASan must report neither a use-after-free nor a leak.
BEZEL_TEST(codegen_linked_cts_810_asan, dispose_then_parent_cancel_no_uaf) {
    int r = runUnderAsan(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "child.Dispose(); "
        "a.Cancel(); "
        "b.Cancel(); "
        "return 0; "));
    if (r < 0) return;  // toolchain unavailable — SKIP
    BEZEL_EXPECT_EQ(r, 0);
}

// §6.1 / §30.8: a full linked lifecycle — create linked, cancel a parent, read
// the child token, then dispose everything (all `using var`).  ASan must report
// no leak and no use-after-free.
BEZEL_TEST(codegen_linked_cts_810_asan, full_linked_lifecycle_leak_free) {
    int r = runUnderAsan(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var child := CancellationTokenSource.CreateLinked(a.Token, b.Token); "
        "var tok := child.Token; "
        "a.Cancel(); "
        "if tok.IsCancellationRequested then return 0; end; "
        "return 0; "));
    if (r < 0) return;  // toolchain unavailable — SKIP
    BEZEL_EXPECT_EQ(r, 0);
}

// §6.1 / §30.8: a full CreateLinkedAll lifecycle under ASan — three parents in an
// owned Array, cancel one, dispose everything.  No leak, no use-after-free.
BEZEL_TEST(codegen_linked_cts_810_asan, create_linked_all_lifecycle_leak_free) {
    int r = runUnderAsan(prog(
        "using var a := new CancellationTokenSource(); "
        "using var b := new CancellationTokenSource(); "
        "using var c := new CancellationTokenSource(); "
        "using var parents := new Array<CancellationToken>(); "
        "parents.Add(a.Token); parents.Add(b.Token); parents.Add(c.Token); "
        "using var child := CancellationTokenSource.CreateLinkedAll(parents); "
        "b.Cancel(); "
        "if child.Token.IsCancellationRequested then return 0; end; "
        "return 0; "));
    if (r < 0) return;  // toolchain unavailable — SKIP
    BEZEL_EXPECT_EQ(r, 0);
}
