// Codegen — OwningArray<T>/OwningLinkedList<T>/OwningStack<T>/OwningQueue<T>.Drain()
// ownership-transferring for-loop (#639).
//
// stdlib §4.5/§4.7/§4.11 v0.135: Drain() returns IOwningEnumerable<T>.
// Iterating transfers ownership of each T to the consumer; the loop variable
// is owned and must be discharged inside the body (§28.11 "iteration order").
//
// Drain order per §28.11 / stdlib §4.5/§4.7/§4.11:
//   OwningArray<T>      — front-to-back (index 0, 1, ...)
//   OwningQueue<T>      — FIFO = front-to-back (dequeue order)
//   OwningStack<T>      — LIFO = top-first (pop order)
//   OwningLinkedList<T> — head-to-tail
//
// Partial drain: break exits; remainder disposed by collection's Dispose().
// After full drain: collection is empty (Count == 0), reusable via Add/Push/etc.
//
// LSan tests confirm no leaks / double-free on partial-drain paths (ASan).

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
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir639() {
    return wtScratchDir("wt-639");
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// The §29.2/#1329 auto-drop tests need the FULL front-end (checkDefiniteAssign-
// ment + checkOwnership) so the ownership pass sets ownedIterVarAutoDropAtBody-
// End on the ForStmt. The AOT driver (runAot -> Driver::compileFile) runs that
// full pipeline — strictly more than the plain JIT harness once did — so runFull
// is now just runAot (kept as a named alias to preserve the call sites and make
// the "needs the ownership pass" intent explicit).
int runFull(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// LSan/ASan helper — compile → object → link with ASan runtime → run.
// Returns 0 (clean), 1 (sanitizer fired), -1 (toolchain missing).
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir639();
    std::string stem = unique_stem("bezel_639_asan");
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

// Shared destructor-counter class template for all tests.
// R._n increments on each disposal; tests encode expected values in return.
std::string makeProgram(const std::string& mainBody) {
    return
        "module M; "
        "class R "
        "  public static _n: int := 0; "
        "  destructor _n := _n + 1; end destructor; "
        "end R; "
        "class C public static method Main(): int "
        + mainBody + " end Main; end C;";
}

}  // namespace

// ===========================================================================
// §4.5 / #639: OwningArray<T>.Drain() — front-to-back order
// ===========================================================================

// §4.5: full drain disposes every element exactly once.
// Three elements added; drain loop uses `using var u := item` to discharge.
// Encoded: R._n after drain == 3.
BEZEL_TEST(owned_drain_639, ownedarray_full_drain_disposes_all_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.5: after full drain the array is empty (Count == 0).
BEZEL_TEST(owned_drain_639, ownedarray_full_drain_empties_array_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  using var u := item; "
        "end; "
        "return arr.Count;"  // 0
    )), 0);
}

// §4.5 / §28.11: drain order is front-to-back.
// Elements added with N values [1,2,3]. Collect them; first should be 1,
// last should be 3. Encode: first*100 + last.
BEZEL_TEST(owned_drain_639, ownedarray_drain_order_front_to_back_spec_4_5) {
    // Sealed class with stored value N.
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var arr := new OwningArray<Val>(); "
        "  arr.Add(new Val(1)); "
        "  arr.Add(new Val(2)); "
        "  arr.Add(new Val(3)); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in arr.Drain() do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "  // 1*100 + 3 = 103
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// §4.5: partial drain (break after first iteration).
// Drained 1 element; remaining 2 stay in array, disposed at scope end.
// Encoded: afterBreak*10 + afterScopeEnd == 1*10 + 3 = 13.
BEZEL_TEST(owned_drain_639, ownedarray_partial_drain_break_remainder_disposed_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.5: empty array drain — loop body never executes.
BEZEL_TEST(owned_drain_639, ownedarray_empty_drain_no_iterations_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "var count := 0; "
        "for var item in arr.Drain() do "
        "  using var u := item; "
        "  count := count + 1; "
        "end; "
        "return count;"  // 0
    )), 0);
}

// §4.5: collection reusable after full drain (Add works, Count increases).
BEZEL_TEST(owned_drain_639, ownedarray_reusable_after_full_drain_spec_4_5) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  using var u := item; "
        "end; "
        "arr.Add(move (new R())); "  // add after drain
        "return arr.Count;"  // 1 — reusable
    )), 1);
}

// ===========================================================================
// §4.7 / #639: OwningLinkedList<T>.Drain() — head-to-tail order
// ===========================================================================

// §4.7: full drain disposes all elements exactly once.
BEZEL_TEST(owned_drain_639, ownedlinkedlist_full_drain_disposes_all_spec_4_7) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var ll := new OwningLinkedList<R>(); "
        "ll.AddLast(move (new R())); "
        "ll.AddLast(move (new R())); "
        "ll.AddLast(move (new R())); "
        "for var item in ll.Drain() do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.7: after full drain the list is empty (Count == 0).
BEZEL_TEST(owned_drain_639, ownedlinkedlist_full_drain_empties_list_spec_4_7) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var ll := new OwningLinkedList<R>(); "
        "ll.AddLast(move (new R())); "
        "ll.AddLast(move (new R())); "
        "for var item in ll.Drain() do "
        "  using var u := item; "
        "end; "
        "return ll.Count;"  // 0
    )), 0);
}

// §4.7 / §28.11: drain order is head-to-tail.
// AddLast [1,2,3]; first drained should be 1 (head), last should be 3 (tail).
BEZEL_TEST(owned_drain_639, ownedlinkedlist_drain_order_head_to_tail_spec_4_7) {
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var ll := new OwningLinkedList<Val>(); "
        "  ll.AddLast(new Val(1)); "
        "  ll.AddLast(new Val(2)); "
        "  ll.AddLast(new Val(3)); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in ll.Drain() do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "  // 1*100 + 3 = 103
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// §4.7: partial drain (break after first element).
BEZEL_TEST(owned_drain_639, ownedlinkedlist_partial_drain_break_remainder_disposed_spec_4_7) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var ll := new OwningLinkedList<R>(); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  for var item in ll.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.7: empty list drain — no iterations.
BEZEL_TEST(owned_drain_639, ownedlinkedlist_empty_drain_no_iterations_spec_4_7) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var ll := new OwningLinkedList<R>(); "
        "var count := 0; "
        "for var item in ll.Drain() do "
        "  using var u := item; "
        "  count := count + 1; "
        "end; "
        "return count;"  // 0
    )), 0);
}

// ===========================================================================
// §4.11 / #639: OwningStack<T>.Drain() — LIFO (top-first) order
// ===========================================================================

// §4.11: full drain disposes all elements exactly once.
BEZEL_TEST(owned_drain_639, ownedstack_full_drain_disposes_all_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningStack<R>(); "
        "s.Push(move (new R())); "
        "s.Push(move (new R())); "
        "s.Push(move (new R())); "
        "for var item in s.Drain() do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.11: after full drain the stack is empty (Count == 0).
BEZEL_TEST(owned_drain_639, ownedstack_full_drain_empties_stack_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningStack<R>(); "
        "s.Push(move (new R())); "
        "s.Push(move (new R())); "
        "for var item in s.Drain() do "
        "  using var u := item; "
        "end; "
        "return s.Count;"  // 0
    )), 0);
}

// §4.11 / §28.11: drain order is LIFO (top-first = last-pushed first).
// Push [1,2,3]; drain should yield 3 first, 1 last.
BEZEL_TEST(owned_drain_639, ownedstack_drain_order_lifo_spec_4_11) {
    // Packed value 301 > 255 → print + runAotOut.
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var s := new OwningStack<Val>(); "
        "  s.Push(new Val(1)); "
        "  s.Push(new Val(2)); "
        "  s.Push(new Val(3)); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in s.Drain() do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  Console.WriteLine(first * 100 + last); return 0; "  // 3*100 + 1 = 301
        "end Main; end C;"), "301");
}

// §4.11: partial drain (break after first element, LIFO so top is drained).
BEZEL_TEST(owned_drain_639, ownedstack_partial_drain_break_remainder_disposed_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R())); "
        "  s.Push(move (new R())); "
        "  s.Push(move (new R())); "
        "  for var item in s.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.11: empty stack drain — no iterations.
BEZEL_TEST(owned_drain_639, ownedstack_empty_drain_no_iterations_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var s := new OwningStack<R>(); "
        "var count := 0; "
        "for var item in s.Drain() do "
        "  using var u := item; "
        "  count := count + 1; "
        "end; "
        "return count;"  // 0
    )), 0);
}

// ===========================================================================
// §4.11 / #639: OwningQueue<T>.Drain() — FIFO (front-first) order
// ===========================================================================

// §4.11: full drain disposes all elements exactly once.
BEZEL_TEST(owned_drain_639, ownedqueue_full_drain_disposes_all_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var q := new OwningQueue<R>(); "
        "q.Enqueue(move (new R())); "
        "q.Enqueue(move (new R())); "
        "q.Enqueue(move (new R())); "
        "for var item in q.Drain() do "
        "  using var u := item; "
        "end; "
        "return R._n;"  // 3
    )), 3);
}

// §4.11: after full drain the queue is empty (Count == 0).
BEZEL_TEST(owned_drain_639, ownedqueue_full_drain_empties_queue_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var q := new OwningQueue<R>(); "
        "q.Enqueue(move (new R())); "
        "q.Enqueue(move (new R())); "
        "for var item in q.Drain() do "
        "  using var u := item; "
        "end; "
        "return q.Count;"  // 0
    )), 0);
}

// §4.11 / §28.11: drain order is FIFO (front-first = first-enqueued first).
// Enqueue [1,2,3]; drain should yield 1 first, 3 last.
BEZEL_TEST(owned_drain_639, ownedqueue_drain_order_fifo_spec_4_11) {
    auto r = runAot(
        "module M; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class C public static method Main(): int "
        "  using var q := new OwningQueue<Val>(); "
        "  q.Enqueue(new Val(1)); "
        "  q.Enqueue(new Val(2)); "
        "  q.Enqueue(new Val(3)); "
        "  var first := -1; "
        "  var last := -1; "
        "  for var item in q.Drain() do "
        "    using var u := item; "
        "    if first = -1 then first := u.N; end; "
        "    last := u.N; "
        "  end; "
        "  return first * 100 + last; "  // 1*100 + 3 = 103
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 103);
}

// §4.11: partial drain (break after first element, FIFO so front is drained).
BEZEL_TEST(owned_drain_639, ownedqueue_partial_drain_break_remainder_disposed_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(move (new R())); "
        "  q.Enqueue(move (new R())); "
        "  q.Enqueue(move (new R())); "
        "  for var item in q.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "  afterBreak := R._n; "  // 1 disposed so far
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §4.11: empty queue drain — no iterations.
BEZEL_TEST(owned_drain_639, ownedqueue_empty_drain_no_iterations_spec_4_11) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var q := new OwningQueue<R>(); "
        "var count := 0; "
        "for var item in q.Drain() do "
        "  using var u := item; "
        "  count := count + 1; "
        "end; "
        "return count;"  // 0
    )), 0);
}

// ===========================================================================
// §4.5 / #639: R27 repro probes — exact match to /tmp/r27/r25*.bzl
// ===========================================================================

// R27 repro r25b_ownedarray_drain.bzl: OwningArray drain, sum should be 60.
// Pass condition: Main returns 42 (42 if sum = 60 else 91).
BEZEL_TEST(owned_drain_639, r27_repro_r25b_ownedarray_drain) {
    auto r = runAot(
        "module R27; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class Program "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Val>(); "
        "    arr.Add(new Val(10)); "
        "    arr.Add(new Val(20)); "
        "    arr.Add(new Val(30)); "
        "    var sum := 0; "
        "    for var item in arr.Drain() do "
        "      using var it := item; "
        "      sum := sum + it.N; "
        "    end; "
        "    return (if sum = 60 then 42 else 91); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// R27 repro r25c_owned_collections_drain.bzl: OwningLinkedList drain, sum=15.
// Pass condition: Main returns 42.
BEZEL_TEST(owned_drain_639, r27_repro_r25c_ownedlinkedlist_drain) {
    auto r = runAot(
        "module R27; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class Program "
        "  public static method Main(): int "
        "    using var ll := new OwningLinkedList<Val>(); "
        "    ll.AddLast(new Val(5)); "
        "    ll.AddLast(new Val(10)); "
        "    var sum := 0; "
        "    for var item in ll.Drain() do "
        "      using var it := item; "
        "      sum := sum + it.N; "
        "    end; "
        "    return (if sum = 15 then 42 else 91); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// R27 repro r25d_ownedstack_drain.bzl: OwningStack drain, sum=15.
// Pass condition: Main returns 42.
BEZEL_TEST(owned_drain_639, r27_repro_r25d_ownedstack_drain) {
    auto r = runAot(
        "module R27; "
        "sealed class Val "
        "  public N: int := 0; "
        "  public constructor(n: int) N := n; end constructor; "
        "end Val; "
        "class Program "
        "  public static method Main(): int "
        "    using var s := new OwningStack<Val>(); "
        "    s.Push(new Val(5)); "
        "    s.Push(new Val(10)); "
        "    var sum := 0; "
        "    for var item in s.Drain() do "
        "      using var it := item; "
        "      sum := sum + it.N; "
        "    end; "
        "    return (if sum = 15 then 42 else 91); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ===========================================================================
// §4.5 / #639: ASan — drain transfers ownership correctly (no double-free/leak)
// ===========================================================================

// ASan: OwningArray full drain — no leak, no double-free.
BEZEL_TEST(owned_drain_639, asan_ownedarray_full_drain_no_leak) {
    int asan = runUnderASan(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  using var u := item; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ASan: OwningArray partial drain — remainder disposed exactly once by array.
BEZEL_TEST(owned_drain_639, asan_ownedarray_partial_drain_no_leak) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ASan: OwningStack partial drain — remainder disposed exactly once by stack.
BEZEL_TEST(owned_drain_639, asan_ownedstack_partial_drain_no_leak) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var s := new OwningStack<R>(); "
        "  s.Push(move (new R())); "
        "  s.Push(move (new R())); "
        "  s.Push(move (new R())); "
        "  for var item in s.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ASan: OwningQueue partial drain — remainder disposed exactly once by queue.
BEZEL_TEST(owned_drain_639, asan_ownedqueue_partial_drain_no_leak) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var q := new OwningQueue<R>(); "
        "  q.Enqueue(move (new R())); "
        "  q.Enqueue(move (new R())); "
        "  q.Enqueue(move (new R())); "
        "  for var item in q.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// ASan: OwningLinkedList partial drain — remainder disposed once by list.
BEZEL_TEST(owned_drain_639, asan_ownedlinkedlist_partial_drain_no_leak) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var ll := new OwningLinkedList<R>(); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  ll.AddLast(move (new R())); "
        "  for var item in ll.Drain() do "
        "    using var u := item; "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}


// ===========================================================================
// §29.2 / §28.3 v0.157 / #1329: per-iteration auto-drop of owned iter vars
// ===========================================================================
//
// The ownership pass now emits a WARNING (not error) for undischarged owned
// iteration variables and sets ownedIterVarAutoDropAtBodyEnd = true on the
// ForStmt. Codegen registers a null-guarded per-iteration disposal scope in
// lowerForBody so the iter var is disposed at every body-exit edge (fall-
// through, break, continue, return) that still holds a live pointer.
// x.Dispose() nulls the slot; `move x` also nulls the slot so no double-free.

// §29.2 / #1329: undischarged drain — auto-drop at body-end each iteration.
// Three elements; loop body does nothing (auto-drop fires). R._n == 3.
// Uses runFull (includes checkOwnership) so ownedIterVarAutoDropAtBodyEnd is set.
BEZEL_TEST(owned_drain_639, autodrop_fallthrough_disposes_all_spec_29_2_1329) {
    BEZEL_EXPECT_EQ(runFull(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "end; "     // body empty — auto-drop fires each iteration
        "return R._n;"  // 3
    )), 3);
}

// §29.2 / #1329: auto-drop on break — the current element must be disposed
// when break exits the loop. Drain 3; break on first → 1 disposed by auto-
// drop, 2 remain in array for collection Dispose. Total at scope-end: 3.
// Uses runFull (includes checkOwnership) so ownedIterVarAutoDropAtBodyEnd is set.
BEZEL_TEST(owned_drain_639, autodrop_break_disposes_current_element_spec_29_2_1329) {
    BEZEL_EXPECT_EQ(runFull(makeProgram(
        "var afterBreak := -1; "
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "    break; "   // item is auto-dropped by emitLoopDisposals
        "  end; "
        "  afterBreak := R._n; "   // 1 disposed (auto-drop on break)
        "end; "
        "return afterBreak * 10 + R._n;"  // 1*10 + 3 = 13
    )), 13);
}

// §29.2 / #1329: explicit x.Dispose() — no double-dispose. The slot is nulled
// by explicitDisposeOwnedReceiverSlot; the auto-drop null-guard skips it.
// Three elements, each explicitly disposed: R._n == 3 exactly.
BEZEL_TEST(owned_drain_639, autodrop_explicit_dispose_no_double_free_spec_29_2_1329) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  item.Dispose(); "   // explicit; slot nulled → auto-drop is no-op
        "end; "
        "return R._n;"  // 3 (each disposed exactly once)
    )), 3);
}

// §29.2 / #1329: `move item` — slot is nulled; auto-drop is no-op.
// Moved into a using-var sink that disposes it. Three elements: R._n == 3.
BEZEL_TEST(owned_drain_639, autodrop_move_out_no_double_free_spec_29_2_1329) {
    BEZEL_EXPECT_EQ(run(makeProgram(
        "using var arr := new OwningArray<R>(); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  using var sink := move item; "  // move nulls slot; using-var disposes
        "end; "
        "return R._n;"  // 3 (each disposed exactly once via using-var)
    )), 3);
}

// §29.2 / #1329 ASan: auto-drop fallthrough — no leak, no double-free.
BEZEL_TEST(owned_drain_639, asan_autodrop_fallthrough_no_leak_spec_29_2_1329) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// §29.2 / #1329 ASan: auto-drop on break — no leak, no double-free.
BEZEL_TEST(owned_drain_639, asan_autodrop_break_no_leak_spec_29_2_1329) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "    break; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// §29.2 / #1329 ASan: explicit dispose + auto-drop null-guard — no double-free.
BEZEL_TEST(owned_drain_639, asan_autodrop_explicit_dispose_no_double_free_spec_29_2_1329) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "for var item in arr.Drain() do "
        "  item.Dispose(); "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}

// §29.2 / #1329 ASan: move-out + auto-drop null-guard — no double-free.
BEZEL_TEST(owned_drain_639, asan_autodrop_move_out_no_double_free_spec_29_2_1329) {
    int asan = runUnderASan(makeProgram(
        "do "
        "  using var arr := new OwningArray<R>(); "
        "  arr.Add(move (new R())); "
        "  arr.Add(move (new R())); "
        "  for var item in arr.Drain() do "
        "    using var sink := move item; "
        "  end; "
        "end; "
        "return 0;"
    ));
    if (asan == -1) return;
    BEZEL_EXPECT_EQ(asan, 0);
}
