// Codegen — disposal of a fresh `new <class>()` temporary used ONLY as the
// receiver of a member access or method call (issue #1039, §28.3 auto-drop).
//
// A fresh owned class instance used only as a receiver — never bound to a
// variable — was owned by nothing and leaked: `new Cell().V` (field read) and
// `new Cell().GetV()` (method call) ran the constructor but never the
// destructor, and never freed the block (LSan: "16 byte(s) leaked"). The fix
// disposes the temporary after the value result has been extracted, but ONLY
// when the result is a NON-REFERENCE value (numeric/bool/char/decimal
// primitive, enum, or unit) — provably free of any reference or borrow into the
// receiver. A `string`/`object`/class/struct result IS (or may embed) a borrow
// of the receiver, so the temporary is NOT disposed there (a leak, but sound —
// never a use-after-free).
//
// Cell's constructor bumps the static counter `n` (+1) and its destructor
// decrements it (-1), so `return Cell.n` after a `do…end` scope is an
// authoritative balance: 0 == disposed exactly once, 1 == leaked (the bug),
// 255 (-1) == disposed twice. The LeakSanitizer leg proves the block is
// reclaimed with no leak AND no double-free / use-after-free (load-bearing —
// the receiver must be freed exactly once, and the EXCLUSION cases must never
// free a still-aliased receiver).

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
#include "support/aot_support.hpp"      // runAot — AOT oracle (JIT->AOT migration)
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
static const std::filesystem::path& wtDir1039() {
    return wtScratchDir("wt-1039");
}

// AOT oracle: fresh-receiver auto-drop (§28.3/§28.7) is disposal codegen, but
// the surrounding programs must first pass the ownership pass the JIT skipped.
int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification (mirrors owned_dict_disposal_test.cpp). Returns
// 0 = clean, 1 = sanitizer reported a problem, -1 = toolchain unavailable.
// detectLeaks=false suppresses leak reports but still flags double-free / UAF.
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir1039();
    std::string stem = unique_stem("bezel_1039_lsan");
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
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
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
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectDisposedAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;        // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);      // 0 == LSan clean (no leak, no UAF/DF)
}

// Cell: ctor +1 / dtor -1 on the static counter; a value field, a value method,
// an enum field and an enum method. `return Cell.n` after a `do…end` scope is
// the disposed-exactly-once balance (0).
std::string prog(const std::string& body) {
    return
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 7; "
        "  public Col: Color := Color.Green; "
        "  public constructor() n := n + 1; end constructor; "
        "  public method GetV(): int return 7; end GetV; "
        "  public method GetCol(): Color return Col; end GetCol; "
        "  destructor n := n - 1; end destructor; "
        "end Cell; "
        "class P public static method Main(): int " + body + " end Main; end P;";
}

// ---- The bug: a fresh `new` receiver leaks (now disposed exactly once). -----

// `new Cell().V` — value field read. Was n=1 (leak); now n=0.
BEZEL_TEST(fresh_owned_receiver_1039, value_field_read_disposed) {
    expectDisposedAndLeakFree(
        prog("do var v: int := new Cell().V; end; return Cell.n;"), 0);
}

// `new Cell().GetV()` — value method call. Was n=1 (leak); now n=0.
BEZEL_TEST(fresh_owned_receiver_1039, value_method_call_disposed) {
    expectDisposedAndLeakFree(
        prog("do var v: int := new Cell().GetV(); end; return Cell.n;"), 0);
}

// `new Cell().V;` — bare expression statement (field). Disposed.
BEZEL_TEST(fresh_owned_receiver_1039, bare_field_stmt_disposed) {
    expectDisposedAndLeakFree(
        prog("do new Cell().V; end; return Cell.n;"), 0);
}

// `new Cell().GetV();` — bare expression statement (method). Disposed.
BEZEL_TEST(fresh_owned_receiver_1039, bare_method_stmt_disposed) {
    expectDisposedAndLeakFree(
        prog("do new Cell().GetV(); end; return Cell.n;"), 0);
}

// An enum result is a value (no reference into the receiver): disposed.
BEZEL_TEST(fresh_owned_receiver_1039, enum_field_and_method_disposed) {
    expectDisposedAndLeakFree(
        prog("do var a: Color := new Cell().Col; "
             "var b: Color := new Cell().GetCol(); end; return Cell.n;"), 0);
}

// Two fresh receivers in one expression — each disposed exactly once (n=0).
BEZEL_TEST(fresh_owned_receiver_1039, multiple_fresh_receivers_disposed) {
    expectDisposedAndLeakFree(
        prog("do var s: int := new Cell().V + new Cell().GetV(); end; "
             "return Cell.n;"), 0);
}

// A fresh receiver in a conditional-expression arm — disposed on the taken arm.
BEZEL_TEST(fresh_owned_receiver_1039, conditional_arm_fresh_receiver_disposed) {
    expectDisposedAndLeakFree(
        prog("do var c: bool := true; "
             "var v: int := (if c then new Cell().V else 0); end; return Cell.n;"),
        0);
}

// ---- Controls: a non-fresh receiver is NEVER disposed by this path. ---------

// A bound `using var` local is disposed exactly once at scope end (the fresh-
// receiver path must not also fire — it would double-free). n=0.
BEZEL_TEST(fresh_owned_receiver_1039, bound_local_receiver_not_double_disposed) {
    expectDisposedAndLeakFree(
        prog("do using var c := new Cell(); "
             "var a: int := c.V; var b: int := c.GetV(); end; return Cell.n;"),
        0);
}

// ---- Exclusions: a reference-typed result must NOT free the receiver. -------
// These receivers intentionally leak (sound); the load-bearing assertion is
// that there is NO use-after-free / double-free (LSan with leak detection off).

// A heap `string` field result borrows the receiver's refcounted string; the
// receiver must NOT be disposed (else the returned string dangles). The program
// must run correctly (no UAF crash) and report no double-free / UAF.
BEZEL_TEST(fresh_owned_receiver_1039, heap_string_field_result_not_disposed) {
    std::string src =
        "module M; "
        "class Cell public static n: int := 0; public Name: string; "
        "  public constructor(x: int) n := n + 1; Name := $\"v{x}w\"; "
        "    end constructor; "
        "  public method GetName(): string return Name; end GetName; "
        "  destructor n := n - 1; end destructor; "
        "end Cell; "
        "class P public static method Main(): int "
        "  do var s: string := new Cell(3).Name; var len: int := s.Length; end; "
        "  return 7; end Main; end P;";
    BEZEL_EXPECT_EQ(run(src), 7);
    int lsan = runUnderLeakSanitizer(src, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);  // no double-free / use-after-free
}

// A class-reference method result borrows a receiver sub-object; the receiver
// must NOT be disposed (else the returned reference dangles). No UAF / DF.
BEZEL_TEST(fresh_owned_receiver_1039, class_reference_result_not_disposed) {
    std::string src =
        "module M; "
        "class Inner public W: int := 5; end Inner; "
        // `In` is `owned` (§28.5): a fresh `new Inner()` may not initialize a
        // non-`owned` (borrow) field — it would leak with no owner (§28.5). The
        // exclusion under test is unchanged: GetIn() returns a *borrow* of the
        // receiver's sub-object, so the fresh receiver must NOT be disposed.
        "class Cell public static n: int := 0; public owned In: Inner := new Inner(); "
        "  public constructor() n := n + 1; end constructor; "
        "  public method GetIn(): Inner return In; end GetIn; "
        "  destructor n := n - 1; end destructor; "
        "end Cell; "
        "class P public static method Main(): int "
        "  do var i: Inner := new Cell().GetIn(); var v: int := i.W; end; "
        "  return 7; end Main; "
        "end P;";
    BEZEL_EXPECT_EQ(run(src), 7);
    int lsan = runUnderLeakSanitizer(src, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);  // no double-free / use-after-free
}

// ---- #1053 case 2: a method-result owned PRODUCER used as a receiver. -------
// `Make(): owned Foo` transfers a fresh +1; used only as a receiver of a value
// member/method, the temporary leaks just like a `new` receiver. It is disposed
// on the SAME non-reference-value restriction. A method WITHOUT an `owned`
// return transfers no ownership (its result is a borrow) and must NEVER be
// disposed (use-after-free / double-free of a value owned elsewhere).

// A Foo with an owned-producer factory, a value field and a value method.
std::string progProducer(const std::string& mainBody) {
    return
        "module M; "
        "class Foo "
        "  public static n: int := 0; "
        "  public V: int := 7; "
        "  public constructor() n := n + 1; end constructor; "
        "  public method GetV(): int return 7; end GetV; "
        "  destructor n := n - 1; end destructor; "
        "end Foo; "
        "class P "
        "  public static method Make(): owned Foo return new Foo(); end Make; "
        "  public static method Main(): int " + mainBody + " end Main; "
        "end P;";
}

// `Make().V` (owned-producer field read) and `Make().GetV()` (method call) —
// each producer temp is disposed exactly once (n=0), leak-free.
BEZEL_TEST(fresh_owned_receiver_1039, owned_producer_value_field_disposed_1053) {
    expectDisposedAndLeakFree(
        progProducer("do var v: int := Make().V; end; return Foo.n;"), 0);
}
BEZEL_TEST(fresh_owned_receiver_1039, owned_producer_value_method_disposed_1053) {
    expectDisposedAndLeakFree(
        progProducer("do var v: int := Make().GetV(); end; return Foo.n;"), 0);
}
BEZEL_TEST(fresh_owned_receiver_1039, owned_producer_two_temps_disposed_1053) {
    expectDisposedAndLeakFree(
        progProducer("do var a: int := Make().V; var b: int := Make().GetV(); "
                     "end; return Foo.n;"), 0);
}

// CRITICAL EXCLUSION: a borrow-returning method (no `owned`) used as a receiver
// must NOT be disposed. `Self(): Node return this;` returns a borrow of the
// receiver; disposing it would double-free the bound `using var`. n=0 with no
// double-free / use-after-free.
BEZEL_TEST(fresh_owned_receiver_1039, borrow_returning_receiver_not_disposed_1053) {
    std::string src =
        "module M; "
        "class Node public static n: int := 0; public V: int := 7; "
        "  public constructor() n := n + 1; end constructor; "
        "  public method Self(): Node return this; end Self; "
        "  destructor n := n - 1; end destructor; "
        "end Node; "
        "class P public static method Main(): int "
        "  do using var nd := new Node(); var v: int := nd.Self().V; end; "
        "  return Node.n; end Main; end P;";
    expectDisposedAndLeakFree(src, 0);  // nd disposed once; Self() borrow not freed
}

// An owned-producer receiver whose result is a heap `string` (borrows the
// producer's refcounted string) must NOT be disposed — no use-after-free.
// (The producer leaks, which is sound; the load-bearing check is no UAF/DF.)
BEZEL_TEST(fresh_owned_receiver_1039, owned_producer_string_result_not_disposed_1053) {
    std::string src =
        "module M; "
        "class Foo public static n: int := 0; public Name: string; "
        "  public constructor() n := n + 1; Name := $\"hi{n}x\"; end constructor; "
        "  public method GetName(): string return Name; end GetName; "
        "  destructor n := n - 1; end destructor; "
        "end Foo; "
        "class P "
        "  public static method Make(): owned Foo return new Foo(); end Make; "
        "  public static method Main(): int "
        "    do var s: string := Make().Name; var len: int := s.Length; end; "
        "    do var t: string := Make().GetName(); var l2: int := t.Length; end; "
        "    return 7; end Main; end P;";
    BEZEL_EXPECT_EQ(run(src), 7);
    int lsan = runUnderLeakSanitizer(src, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);  // no double-free / use-after-free
}

// ---- #1053 case 1: explicit `.Dispose()` on a fresh UNBOUND producer. -------
// `new Cell().Dispose()` ran the synthesised destructor (n=0) but never
// reclaimed the unbound heap block (LSan: "16 byte(s) leaked") — the explicit-
// dispose path stayed dispatch-only for a receiver with no owning slot. Now a
// fresh producer (NewExpr / `owned`-returning call) is reclaimed there exactly
// once (the sole dispatch + the block free). A BOUND `var c := new X();
// c.Dispose()` has a variable receiver (an owning slot) and is reclaimed by the
// existing slot path — unaffected (no double-free).

// `new Cell().Dispose()` — destructor exactly once AND block reclaimed.
BEZEL_TEST(fresh_owned_receiver_1039, new_dispose_unbound_reclaimed_1053) {
    expectDisposedAndLeakFree(
        prog("do new Cell().Dispose(); end; return Cell.n;"), 0);
}

// `Make().Dispose()` — owned-producer receiver, explicit Dispose, reclaimed.
BEZEL_TEST(fresh_owned_receiver_1039, owned_producer_dispose_reclaimed_1053) {
    expectDisposedAndLeakFree(
        progProducer("do Make().Dispose(); end; return Foo.n;"), 0);
}

// CONTROL: a BOUND local explicitly disposed is reclaimed exactly once by the
// slot path (the fresh-producer reclaim must NOT also fire — variable receiver).
BEZEL_TEST(fresh_owned_receiver_1039, bound_local_dispose_single_free_1053) {
    expectDisposedAndLeakFree(
        prog("do var c := new Cell(); c.Dispose(); end; return Cell.n;"), 0);
}

}  // namespace
