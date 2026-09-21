// Codegen + runtime — a collection literal in CALL-ARGUMENT position
// (issue #999, spec §15.4, v0.148). §15.4 admits a collection literal "via the
// parameter type of a method call", so `foo([1, 2, 3])` where foo's parameter is
// `Array<int>` is NORMATIVE syntax that MUST CONSTRUCT the collection from the
// literal. A CollectionLiteralExpr is not a general expression value (lowerExpr
// returns null for it); the local-decl / return / field-init paths each
// materialize it via a dedicated path, but the call-argument path had none — so
// the literal lowered to null/garbage. Ordinary methods silently received an
// EMPTY collection (WRONG_VALUE: Count 0, sum 0); the byte-array FFI factories
// (RawBytes.From / CString.FromBytes) dereferenced the garbage and CRASHED.
//
// The fix materializes the literal into an owned temporary of the parameter's
// target type (reusing the SAME materializeHeapCollectionLiteral machinery) and
// disposes the temp per the parameter's mode (§28.3):
//   - BORROW parameter (`a: Array<int>`): the temp is an owned local disposed
//     exactly once at scope exit, on the normal AND unwind paths.
//   - `owned` parameter (`owned a: OwningArray<int>`): ownership transfers into
//     the callee — the caller registers NO disposal (no double-free).
//
// These tests verify (1) ordinary-method value correctness (Count / for-in sum /
// string-element count), (2) the FFI-factory case no longer crashes and the
// length is correct (RawBytes), (3) the owned-parameter transfer is correct and
// double-free-free, and (4) ASan dispose-exactly-once for the materialized temp
// across borrow / string-element / owned / unwind / FFI shapes.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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
static const std::filesystem::path& wtDir999() {
    return wtScratchDir("wt-999");
}

// Compile + run natively; return exit code or -1 on failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir999();
    std::string stem = unique_stem("bezel_999");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
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

// Compile + link against the ASan runtime. Returns false on failure/unavailable.
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir999();
    std::string stem = unique_stem("bezel_999_san");
    obj = (dir / (stem + ".o")).string();
    exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) { bezel::test::asanBuildFailure("parse"); return false; }
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) { bezel::test::asanBuildFailure("sema"); return false; }
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) { bezel::test::asanBuildFailure("emitModule"); return false; }
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, obj, err)) {
            bezel::test::asanBuildFailure("emitObjectFile");
            return false;
        }
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + obj + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(obj.c_str()); return false;
    }
    return true;
}

// LSan verification (detect_leaks=1). Returns 0 = clean, 1 = leak/corruption,
// -1 = skip (ASan runtime unavailable).
int runUnderLeakSanitizer(const std::string& source) {
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return -1;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert correct exit code AND LSan-clean (no leak, no UAF/double-free): the
// materialized temp is disposed EXACTLY ONCE.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert correct exit code AND no ASan MEMORY-SAFETY error (UAF/double-free),
// tolerating a residual leak (detect_leaks=0). Used for the FFI-factory shape
// where the source byte array may leak orthogonally (pre-existing #998) — the
// point of THIS test is that the materialized temp is not double-freed.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ---- Programs --------------------------------------------------------------

// §15.4: a collection literal as the argument of a method whose parameter is a
// BORROW `Array<int>`. Count must be 3 (was 0 — empty collection miscompile).
const char* kBorrowArrayCount =
    "module M; class C "
    "  public static method Cnt(a: Array<int>): int return a.Count; end Cnt; "
    "  public static method Main(): int return C.Cnt([10, 20, 30]); end Main; "
    "end C;";

// §15.4 / §29.2: a literal arg consumed by a `for ... in` sum. Sum must be 60
// (was 0 — empty collection iterated zero times).
const char* kBorrowArraySum =
    "module M; class C "
    "  public static method Sum(a: Array<int>): int "
    "    var t: int := 0; for var x in a do t := t + x; end; return t; "
    "  end Sum; "
    "  public static method Main(): int return C.Sum([10, 20, 30]); end Main; "
    "end C;";

// §15.4 / §28.9: a STRING-element literal arg. Each element is materialized and
// retained correctly; the borrow temp + its co-owned +1 per element dispose
// exactly once. Sum of lengths is 2 + 3 = 5.
const char* kBorrowStringArray =
    "module M; class C "
    "  public static method Cnt(a: Array<string>): int "
    "    var n: int := 0; for var s in a do n := n + s.Length; end; return n; "
    "  end Cnt; "
    "  public static method Main(): int return C.Cnt([\"aa\", \"bbb\"]); end Main; "
    "end C;";

// §15.4: a `long`-element literal — the element coercion widens i32 → i64 in the
// materializer; Count must still be 4.
const char* kBorrowLongArray =
    "module M; class C "
    "  public static method Cnt(a: Array<long>): int return a.Count; end Cnt; "
    "  public static method Main(): int return C.Cnt([1, 2, 3, 4]); end Main; "
    "end C;";

// §15.4: the sequence-literal form targets every sequence-shaped collection. A
// literal arg to a Set parameter dedups (3), to a Stack/Queue keeps order, to a
// LinkedList appends — all via the same materializer the local-decl path uses.
const char* kBorrowSet =
    "module M; class C "
    "  public static method Cnt(a: Set<int>): int return a.Count; end Cnt; "
    "  public static method Main(): int return C.Cnt([1, 2, 3, 2]); end Main; "
    "end C;";
const char* kBorrowQueue =
    "module M; class C "
    "  public static method Cnt(a: Queue<int>): int return a.Count; end Cnt; "
    "  public static method Main(): int return C.Cnt([1, 2, 3, 4]); end Main; "
    "end C;";
const char* kBorrowLinkedList =
    "module M; class C "
    "  public static method Cnt(a: LinkedList<int>): int return a.Count; end Cnt; "
    "  public static method Main(): int return C.Cnt([1, 2]); end Main; "
    "end C;";

// §15.4: the dictionary-literal form `[k: v, …]` targets Dictionary<K,V> when it
// is the parameter type. Count is 2 (two distinct keys).
const char* kBorrowDict =
    "module M; class C "
    "  public static method Cnt(a: Dictionary<string, int>): int "
    "    return a.Count; "
    "  end Cnt; "
    "  public static method Main(): int "
    "    return C.Cnt([\"a\": 1, \"b\": 2]); "
    "  end Main; "
    "end C;";

// §15.4 / §28.3: an `owned OwningArray<N>` parameter. Ownership TRANSFERS from
// the materialized temp into the callee (no caller disposal — no double-free).
// The callee discharges it via `using var`. Count must be 3.
// (Migrated from OwningArray<int> → OwningArray<N> per stdlib §4.2/#1327.)
const char* kOwnedArrayTransfer =
    "module M; "
    "class N public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; end N; "
    "class C "
    "  public static method Take(owned a: OwningArray<N>): int "
    "    using var local: OwningArray<N> := a; return local.Count; "
    "  end Take; "
    "  public static method Main(): int "
    "    return C.Take([new N(1), new N(2), new N(3)]); "
    "  end Main; "
    "end C;";

// stdlib §10.3 / §15.4 (the #999 CRASH): RawBytes.From([…]) — a literal arg to a
// byte-array FFI factory (borrow `Array<byte>`). Previously SEGV'd on a garbage
// dereference; now materializes a real Array<byte> and the Length (= count) is 4.
const char* kRawBytesFromLiteral =
    "module M; class C "
    "  public static method Main(): int "
    "    using var r: RawBytes := RawBytes.From([10, 20, 30, 40]); "
    "    return r.Length; "
    "  end Main; "
    "end C;";

// ---- ASan dispose-once programs (looped to amplify any leak) ----------------

// §15.4 / §28.3: borrow-arg literal in a 1000-iteration loop — the materialized
// temp is disposed exactly once per iteration (no leak, no double-free).
const char* kBorrowLoopLeakFree =
    "module M; class C "
    "  public static method Cnt(a: Array<int>): int return a.Count; end Cnt; "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do t := t + C.Cnt([10, 20, 30]); i := i + 1; end; "
    "    if t = 3000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §15.4 / §28.9: string-element borrow-arg literal looped — each element's +1 is
// released with the temp at scope-end; no per-iteration heap growth.
const char* kBorrowStringLoopLeakFree =
    "module M; class C "
    "  public static method Cnt(a: Array<string>): int "
    "    var n: int := 0; for var s in a do n := n + s.Length; end; return n; "
    "  end Cnt; "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do t := t + C.Cnt([\"aa\", \"bbb\"]); i := i + 1; end; "
    "    if t = 5000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §15.4 / §28.3: owned-arg literal transfer looped — the callee disposes the
// transferred collection exactly once; the caller does NOT (no double-free).
// (Migrated from OwningArray<int> → OwningArray<N> per stdlib §4.2/#1327.)
// §28.10 (v0.180, #2071): the adoption uses the binding-position `owned` marker
// (`:= owned a`). An UNMARKED `using var local := a;` is a borrow-pin (§28.10
// v0.180, #2071) — `a` retains the obligation and auto-disposes, which would
// cause the looped disposal pattern to leak (a disposed once via the loop's
// auto-drop path is enough; the marked-form adoption isolates disposal to the
// using-var so the loop can reuse the same caller-side argument pattern).
const char* kOwnedLoopLeakFree =
    "module M; "
    "class N public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; end N; "
    "class C "
    "  public static method Take(owned a: OwningArray<N>): int "
    "    using var local: OwningArray<N> := owned a; return local.Count; "
    "  end Take; "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      t := t + C.Take([new N(1), new N(2), new N(3)]); "
    "      i := i + 1; "
    "    end; "
    "    if t = 3000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §15.4 / §28.3 (UNWIND disposal): the callee THROWS after the call point; the
// materialized borrow temp must be disposed on the exception-unwind path — once,
// no leak, no double-free.
const char* kUnwindDisposeLeakFree =
    "module M; class C "
    "  public static method Boom(a: Array<int>): int "
    "    throw new InvalidOperationException(\"x\"); "
    "  end Boom; "
    "  public static method Run(): int "
    "    try return C.Boom([10, 20, 30]); "
    "    catch InvalidOperationException => return 7; end; "
    "  end Run; "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do t := t + C.Run(); i := i + 1; end; "
    "    if t = 7000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// stdlib §10.3 / §15.4: RawBytes.From literal arg looped under ASan. The
// materialized Array<byte> temp must not be double-freed. (The factory's source
// array may leak orthogonally — pre-existing #998 — so this uses the
// no-corruption variant: tolerate a residual leak, catch any UAF/double-free.)
const char* kRawBytesFromLoopNoCorruption =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var r: RawBytes := RawBytes.From([10, 20, 30, 40]); "
    "      t := t + r.Length; i := i + 1; "
    "    end; "
    "    if t = 4000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- Value correctness (the #999 WRONG_VALUE / CRASH cases) -----------------

// §15.4: ordinary borrow method receives a real (non-empty) collection — Count 3.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_array_count_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowArrayCount), 3);
}

// §15.4 / §29.2: for-in sum over a literal arg — 60.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_array_for_in_sum_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowArraySum), 60);
}

// §15.4 / §28.9: string-element literal arg — sum of lengths 5, strings correct.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_string_array_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowStringArray), 5);
}

// §15.4: long-element literal arg — element coercion i32→i64, Count 4.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_long_array_count_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowLongArray), 4);
}

// §15.4: sequence-literal form targets Set (dedup → 3).
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_set_dedup_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowSet), 3);
}

// §15.4: sequence-literal form targets Queue (4).
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_queue_count_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowQueue), 4);
}

// §15.4: sequence-literal form targets LinkedList (2).
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_linkedlist_count_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowLinkedList), 2);
}

// §15.4: dictionary-literal form targets Dictionary<K,V> (2).
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_dict_count_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kBorrowDict), 2);
}

// §15.4 / §28.3: owned-parameter transfer — Count 3, no double-free.
BEZEL_TEST(codegen_collection_literal_arg_999, owned_array_transfer_28_3) {
    BEZEL_EXPECT_EQ(compileAndRun(kOwnedArrayTransfer), 3);
}

// stdlib §10.3 / §15.4: the #999 CRASH — RawBytes.From([…]).Length is 4 (no SEGV).
BEZEL_TEST(codegen_collection_literal_arg_999, rawbytes_from_literal_length_15_4) {
    BEZEL_EXPECT_EQ(compileAndRun(kRawBytesFromLiteral), 4);
}

// ---- ASan dispose-exactly-once (no leak, no double-free) --------------------

// §15.4 / §28.3: borrow-arg literal temp disposed once — LSan-clean.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_loop_leak_free_28_3) {
    expectValueAndLeakFree(kBorrowLoopLeakFree, 0);
}

// §15.4 / §28.9: string-element borrow temp + per-element +1 disposed once.
BEZEL_TEST(codegen_collection_literal_arg_999, borrow_string_loop_leak_free_28_9) {
    expectValueAndLeakFree(kBorrowStringLoopLeakFree, 0);
}

// §15.4 / §28.3: owned-arg transfer — callee disposes once, caller does not.
BEZEL_TEST(codegen_collection_literal_arg_999, owned_loop_leak_free_28_3) {
    expectValueAndLeakFree(kOwnedLoopLeakFree, 0);
}

// §15.4 / §28.3: unwind disposal — borrow temp disposed on the exception path.
BEZEL_TEST(codegen_collection_literal_arg_999, unwind_dispose_leak_free_28_3) {
    expectValueAndLeakFree(kUnwindDisposeLeakFree, 0);
}

// stdlib §10.3 / §15.4: FFI-factory literal arg — no double-free of the temp.
BEZEL_TEST(codegen_collection_literal_arg_999, rawbytes_from_loop_no_corruption_15_4) {
    expectValueAndNoCorruption(kRawBytesFromLoopNoCorruption, 0);
}
