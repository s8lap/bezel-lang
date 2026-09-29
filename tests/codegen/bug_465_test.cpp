// Fix verification tests for bug #465.
//
// #465 (CRASH / malformed IR): a collection literal `[e1, e2, …]` assigned to a
// variable with an explicit HEAP-collection annotation (`Array<T>`, etc.) emitted
// malformed IR and the compiler then aborted at IR-verify (and SIGSEGV'd).
//
//   module M; class C public static method Main(): int
//     var a: Array<int> := [1, 2, 3];   // → "Invalid indices for GEP pointer
//     return a.Length;                  //    type! ... getelementptr ptr, ..."
//   end Main; end C;
//
//   Root cause (confirmed): lowerLocalDecl (LlvmBackend_Setup.cpp) handled a
//   collection-literal initializer with a SINGLE inline-array store path — it
//   `lowerType(declType)`'d the target and emitted a 2-index GEP
//   (`getelementptr T, ptr, i32 0, i32 N`) into a flat `[N x elem]` memory
//   layout. That is correct for an INLINE array `[N]T` (which lowers to an LLVM
//   ArrayType). But a heap `Array<T>` (§15.2 / stdlib §4.4) lowers to a bare
//   `ptr` — a pointer to a runtime bz_array_t whose elements live in a separately
//   allocated buffer — so the 2-index GEP is malformed IR and the always-on
//   verifier aborts.
//
//   Fix (LlvmBackend_Setup.cpp): when the collection literal's resolved target is
//   a heap, array-backed sequence collection (Array<T> / OwningArray<T> / Stack /
//   OwningStack / Queue / OwningQueue), MATERIALIZE a real heap collection via the
//   runtime — the SAME bz_array_alloc the `new Array<T>()` path emits plus the
//   SAME bz_array_add the `.Add(...)` path emits — and store the resulting
//   pointer. The inline-array path is unchanged for `[N]T` targets.
//
//   Spec: §15.4 (collection literals), §15.2 (heap arrays), stdlib §4.4 (Array<T>),
//   §4.5 (OwningArray<T>), §28.3 / §28.10 (ownership/disposal of the owned result).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

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

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #465 core: the exact verified reproducer ─────────────────────────────────

// §15.4 / §15.2 / stdlib §4.4 / #465: `var a: Array<int> := [1,2,3]; a.Length`.
// Before fix: malformed IR → verifier abort → SIGSEGV. Expected: 3.
BEZEL_TEST(bug465_heap_array_literal, array_int_literal_length_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  return a.Length; "   // expect 3
        "end Main; end C;"), 3);
}

// stdlib §4.4 / §15.5 / #465: Array<T> exposes .Count; for the literal it equals
// the element count (3).
BEZEL_TEST(bug465_heap_array_literal, array_int_literal_count_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [4, 5, 6, 7]; "
        "  return a.Count; "   // expect 4
        "end Main; end C;"), 4);
}

// §15.4 / stdlib §4.4 / #465: element values are stored in textual order — read
// index 0 (== 1) and sum all (== 6).
BEZEL_TEST(bug465_heap_array_literal, array_int_literal_index0_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  return a[0]; "   // expect 1 — first literal element at index 0
        "end Main; end C;"), 1);
}

BEZEL_TEST(bug465_heap_array_literal, array_int_literal_sum_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  return a[0] + a[1] + a[2]; "   // expect 6
        "end Main; end C;"), 6);
}

// §15.4 / stdlib §4.4 / #465: the Get(i) method (the indexer's named form).
BEZEL_TEST(bug465_heap_array_literal, array_int_literal_get_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := [10, 20, 30]; "
        "  return a.Get(2); "   // expect 30
        "end Main; end C;"), 30);
}

// ── #465: using var (ownership/disposal of the owned literal result) ──────────

// §15.4 / §28.10 / #465: `using var a: Array<int> := [..]` — must compile, run,
// and dispose the array once at scope end without crashing.
BEZEL_TEST(bug465_heap_array_literal, using_var_array_int_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a: Array<int> := [1, 2, 3]; "
        "  return a[1]; "   // expect 2
        "end Main; end C;"), 2);
}

// ── #465: element types beyond int ───────────────────────────────────────────

// stdlib §4.4 / §4.6 / #465: Array<string> — reference (pointer) elements. Read
// the array length and a stored string's own Length.
BEZEL_TEST(bug465_heap_array_literal, array_string_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<string> := [\"ab\", \"cde\", \"f\"]; "
        "  return a.Length + a[1].Length; "   // 3 + 3 = 6
        "end Main; end C;"), 6);
}

// stdlib §4.4 / #465: Array<bool> — i1 elements (1-byte slots).
BEZEL_TEST(bug465_heap_array_literal, array_bool_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<bool> := [true, false, true]; "
        "  if a[0] and a[2] and (a[1] = false) then return 1; end; "
        "  return 0; "   // expect 1
        "end Main; end C;"), 1);
}

// §5.1 / stdlib §4.4 / #465: Array<long> — i32 literals widened to i64 slots.
BEZEL_TEST(bug465_heap_array_literal, array_long_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<long> := [10, 20, 30]; "
        "  var s: long := a[0] + a[1] + a[2]; "
        "  if s = 60 then return 1; end; "
        "  return 0; "   // expect 1
        "end Main; end C;"), 1);
}

// §9 / stdlib §4.4 / #465: Array<Record> — value-typed (struct) elements stored
// inline. (Point fits the array's element slot; see bug_464_test for the .Add
// form this materialization reuses.)
BEZEL_TEST(bug465_heap_array_literal, array_record_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; record P(x: int, y: int); "
        "class C public static method Main(): int "
        "  var a: Array<P> := [new P(1, 2), new P(3, 4)]; "
        "  return a[0].x + a[1].y; "   // 1 + 4 = 5
        "end Main; end C;"), 5);
}

// §12 / stdlib §4.4 / #465: Array<IInterface> — interface (pointer) elements;
// virtual dispatch through a stored element works.
BEZEL_TEST(bug465_heap_array_literal, array_interface_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; interface IShape method Area(): int; end IShape; "
        "class F : IShape public method Area(): int return 42; end Area; end F; "
        "class C public static method Main(): int "
        "  var a: Array<IShape> := [new F(), new F()]; "
        "  return a[0].Area() + a.Length; "   // 42 + 2 = 44
        "end Main; end C;"), 44);
}

// ── #465: empty literal (§15.4: admitted for every collection target) ─────────

// §15.4 (empty literals) / #465: `var a: Array<int> := []` — an empty heap array
// with Count/Length 0.
BEZEL_TEST(bug465_heap_array_literal, array_int_empty_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := []; "
        "  return a.Length; "   // expect 0
        "end Main; end C;"), 0);
}

// §15.4 / #465: an empty heap-array literal is a valid, mutable Array — Add then
// read back, proving the materialized object is a real bz_array_t.
BEZEL_TEST(bug465_heap_array_literal, array_int_empty_literal_then_add_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<int> := []; "
        "  a.Add(7); a.Add(8); "
        "  return a.Length + a[0] + a[1]; "   // 2 + 7 + 8 = 17
        "end Main; end C;"), 17);
}

// ── #465: other array-backed sequence targets (§15.4 order semantics) ─────────

// §15.4 (Stack order) / stdlib §4.11 / #465: a Stack<int> literal — the last
// literal element is on top; Pop returns it.
BEZEL_TEST(bug465_heap_array_literal, stack_int_literal_pop_top_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var s: Stack<int> := [1, 2, 3]; "
        "  return s.Pop(); "   // expect 3 (top)
        "end Main; end C;"), 3);
}

// §15.4 (Queue order) / stdlib §4.12 / #465: a Queue<int> literal — the first
// literal element is at the front; Dequeue returns it.
BEZEL_TEST(bug465_heap_array_literal, queue_int_literal_dequeue_front_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var q: Queue<int> := [7, 8, 9]; "
        "  return q.Dequeue(); "   // expect 7 (front)
        "end Main; end C;"), 7);
}

// ── #465: nested heap-array literals ─────────────────────────────────────────

// §15.4 / stdlib §4.4 / #465: a nested Array<Array<int>> literal — each inner
// `[...]` is itself materialized as a heap Array<int> element.
BEZEL_TEST(bug465_heap_array_literal, nested_array_array_int_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Array<int>> := [[1, 2], [3, 4, 5]]; "
        "  return a[0].Length + a[1].Length; "   // 2 + 3 = 5
        "end Main; end C;"), 5);
}

BEZEL_TEST(bug465_heap_array_literal, nested_array_array_int_element_index_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: Array<Array<int>> := [[1, 2], [3, 4, 5]]; "
        "  return a[1][2]; "   // expect 5
        "end Main; end C;"), 5);
}

// ── #465: other collection-literal target sites (same root cause) ────────────

// §15.4 / stdlib §4.4 / #465: a class FIELD initialized by a heap-array literal
// (`public a: Array<int> := [1,2,3];`). Same root cause as the local-var path:
// the field slot is a `ptr`, not an inline `[N x elem]` array. Before fix the
// field was left null → crash on first use. Expected: Length 3 + a[2] 3 = 6.
BEZEL_TEST(bug465_heap_array_literal, field_init_array_int_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public a: Array<int> := [1, 2, 3]; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c.a.Length + c.a[2]; "   // 3 + 3 = 6
        "  end Main; end C;"), 6);
}

// §15.4 / §28.3 / stdlib §4.4 / #465: `return [e1, …]` whose declared return type
// is a heap Array<T>. lowerExpr returns null for a collection literal, so the
// return would otherwise yield a null array (caller crashes). The owned result
// transfers to the caller. Expected: a[0] 9 + Length 3 = 12.
BEZEL_TEST(bug465_heap_array_literal, return_array_int_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): Array<int> return [9, 8, 7]; end Make; "
        "  public static method Main(): int "
        "    using var a := Make(); "
        "    return a[0] + a.Length; "   // 9 + 3 = 12
        "  end Main; end C;"), 12);
}

// §15.4 / §28.3 / #465: `return [..]` with an explicit `owned Array<T>` return.
BEZEL_TEST(bug465_heap_array_literal, return_owned_array_int_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Make(): owned Array<int> "
        "    return [9, 8, 7]; end Make; "
        "  public static method Main(): int "
        "    using var a := Make(); "
        "    return a.Length; "   // expect 3
        "  end Main; end C;"), 3);
}

// ── #465 regression: the INLINE-array form must still work ────────────────────

// §15.1 / #465: `var a: [3]int := [1,2,3]` — the inline-array (value-typed, flat
// `[N x elem]` alloca) path is unchanged by the fix.
BEZEL_TEST(bug465_heap_array_literal, regression_inline_array_literal_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [3]int := [1, 2, 3]; "
        "  return a[0] + a[1] + a[2]; "   // expect 6
        "end Main; end C;"), 6);
}

// §15.1 / §15.5 / #465: inline-array `.Length` is a compile-time constant — still
// resolves correctly (the inline path was never the broken one).
BEZEL_TEST(bug465_heap_array_literal, regression_inline_array_length_465) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: [4]int := [10, 20, 30, 40]; "
        "  return a.Length; "   // expect 4
        "end Main; end C;"), 4);
}

// ════════════════════════════════════════════════════════════════════════════
// #465 disposal correctness — native build + LeakSanitizer.
//
// The materialized heap literal is an OWNED value (§15.4 / §28.3): a `using var`
// must dispose it exactly once at scope end — no leak, no double-free. Mirrors
// free_on_dispose_test.cpp: a destructor-count exit code proves each element is
// disposed once; an out-of-band LeakSanitizer run proves the backing is freed
// once (no UAF/DF). The JIT harness cannot host LSan (its in-process ORC state
// is intentionally never freed), so this leg builds a standalone native binary.
// ════════════════════════════════════════════════════════════════════════════

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir465() {
    return wtScratchDir("wt-465");
}

// Compile `source` to a native executable and return its exit code (-1 on any
// front-end / link / run failure). Same shape as free_on_dispose_test.cpp.
int compileAndRun465(const std::string& source) {
    auto dir = wtDir465();
    std::string stem = unique_stem("bezel_465");
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

// Lower `source` to an object, link against the ASan-instrumented runtime, and
// run under LeakSanitizer. Returns 0 (clean), 1 (sanitizer found a problem), or
// -1 (toolchain unavailable → skip). Same shape as free_on_dispose_test.cpp.
int runUnderLeakSanitizer465(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir465();
    std::string stem = unique_stem("bezel_465_lsan");
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
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

void expectDisposedAndLeakFree465(const std::string& source, int expectedExit) {
    BEZEL_EXPECT_EQ(compileAndRun465(source), expectedExit);
    int lsan = runUnderLeakSanitizer465(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// §15.4 / §28.10 / stdlib §4.5 / #465: a `using var OwningArray<Res>` literal of
// class elements — disposing the array once disposes each element's destructor
// exactly once (count == 3), and the backing + element blocks are reclaimed
// (LSan clean: no leak, no double-free).
const char* kOwnedArrayLiteralDispose =
    "module M; class Res "
    "  public static count: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "  public static method Main(): int "
    "    do using var a: OwningArray<Res> := "
    "         [new Res(1), new Res(2), new Res(3)]; end; "
    "    return Res.count; "
    "  end Main; "
    "end Res;";

// §15.4 / §28.10 / stdlib §4.4 / #465: a `using var Array<int>` literal — int
// elements have no destructors, so this checks the BACKING is freed exactly once
// (LSan clean) with no crash. Exit code 0.
const char* kBorrowingArrayLiteralDispose =
    "module M; class C "
    "  public static method Main(): int "
    "    do using var a: Array<int> := [1, 2, 3]; end; "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

BEZEL_TEST(bug465_heap_array_literal, owned_array_literal_disposes_each_once_465) {
    expectDisposedAndLeakFree465(kOwnedArrayLiteralDispose, 3);
}

BEZEL_TEST(bug465_heap_array_literal, borrowing_array_literal_no_leak_465) {
    expectDisposedAndLeakFree465(kBorrowingArrayLiteralDispose, 0);
}
