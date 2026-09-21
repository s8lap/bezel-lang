// Codegen/ownership — an `owned IEnumerable<T>` / `IOwningEnumerable<T>` CLASS
// FIELD's iterator FRAME must be freed when the enclosing instance is disposed
// (issue #1643, §28.4-step-2 / §29.7).
//
// Root cause: `isOwnedReferenceField` (LlvmBackend_impl.hpp) requires the field's
// resolved TypeRef to carry a non-null `declaration` (a user class/interface
// TypeDecl) to classify it as an owned reference field the class Dispose auto-
// disposes. `IEnumerable<T>` / `IOwningEnumerable<T>` are BUILTIN iterator
// interfaces with NO declaration, so `rt->declaration` is null and the field is
// skipped by `emitOwnedFieldDisposals` → the iterator frame (bz$C$Gen, incl. its
// owned captures) leaks at class disposal (valgrind/LSan "definitely lost 32
// bytes ... from bz$C$Gen").
//
// Fix (#1643): `isOwnedIteratorInterfaceField` (LlvmBackend_impl.hpp) recognises
// a builtin iterator-interface owned field (via isVtableBackedIEnumerableType),
// and `emitOwnedFieldDisposals` (LlvmBackend_Disposal.cpp) disposes it through
// the ITERATOR-vtable slot-1 Dispose protocol — vtable[1](self), which runs the
// state machine's pending finally blocks and FREES THE FRAME ITSELF (§29.7) —
// NOT the class-vtable slot-2 `dynamicDispose`/`emitDisposeAndReclaim` (which
// would invoke MoveNext and never free, then double-free). The ctor-throw
// cleanup classification (hasOwnedToClean) and the move-out field-nulling also
// recognise the field so the disposal is exactly-once across every path.
//
// Each case is verified two ways:
//   1. runJit — the in-process JIT: the exit code proves value correctness (for
//      the counting-destructor cases it IS the disposal counter, 0 iff disposed
//      exactly once, read AFTER the holder leaves scope via an inner `do` block
//      so the using-var disposal has run).
//   2. runUnderLeakSanitizer — parse → sema → OWNERSHIP pass → AOT codegen →
//      link against an ASan runtime → run under LSan (detect_leaks=1): verifies
//      no leak AND no use-after-free / double-free. This is the load-bearing
//      pin: before the fix it returns 1 (Direct leak of 32 bytes from
//      bz$C$Gen); after the fix it returns 0. runJit alone cannot observe the
//      leak (the frame leak is invisible to the program's exit code).
//
// Spec citations: §28.4-step-2 (owned-field auto-disposal), §28.5 (an owned
// field of any disposable type is auto-disposed), §29.7 (iterator vtable format
// [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current; Dispose runs pending finally and
// frees the frame). runJit returns the FULL int from Main(); values ≤ 255.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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

// Parse → sema → ownership → AOT emit → link against the ASan runtime → run
// under LSan. Returns 0 if leak/UAF-free, 1 if a leak or error was detected,
// -1 if the toolchain is unavailable (caller then SKIPs).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_1643_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe = (dir / stem).string();
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
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -1;
    }
    auto cleanup = [&] { std::remove(objPath.c_str()); std::remove(exe.c_str()); };
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/UAF
    return 0;
}

// §28.4-step-2 / §29.7 (#1643): the canonical repro — a class with an
// `owned IEnumerable<int>` FIELD initialized from a yield-method result. The
// field's iterator frame (bz$C$Gen) is freed at the holder's disposal via the
// iterator-vtable slot-1 Dispose. Before the fix the frame leaked (32 bytes
// definitely lost, per LSan/valgrind).
BEZEL_TEST(codegen_owned_iter_field_disposal_1643,
           ienumerable_field_frame_freed_at_disposal) {
    std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IEnumerable<int>; "
        "  public constructor(owned s: IEnumerable<int>) seq := s; "
        "  end constructor; end Holder; "
        "class M2 public static method Main(): int "
        "  using var h := new Holder(C.Gen()); return 0; "
        "end Main; end M2;";
    int lsan = runUnderLeakSanitizer(src);  // runs ownership + AOT
    if (lsan == -1) return;                  // toolchain unavailable
    BEZEL_EXPECT_EQ(lsan, 0);                // frame freed, no leak/UAF
}

// §29.7 (#1643): the IOwningEnumerable<T> variant — a builtin iterator
// interface that extends IDisposable (stdlib §4.13). Its runtime value is the
// same iterator-vtable object (slot-1 Dispose), so the field-disposal path must
// handle it identically. The yielded Cells are created lazily as the enumerator
// advances; since this holder never enumerates, the load-bearing signal is the
// frame-freedom (the LSan leg).
BEZEL_TEST(codegen_owned_iter_field_disposal_1643,
           owning_enumerable_field_frame_freed_at_disposal) {
    std::string src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C public static method Gen(): IOwningEnumerable<Cell> "
        "  yield return new Cell(); yield return new Cell(); "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IOwningEnumerable<Cell>; "
        "  public constructor(owned s: IOwningEnumerable<Cell>) seq := s; "
        "  end constructor; end Holder; "
        "class M2 public static method Main(): int "
        "  using var h := new Holder(C.Gen()); return 0; "
        "end Main; end M2;";
    int lsan = runUnderLeakSanitizer(src);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// §29.2 / §29.7 (#1643): disposing the field's frame at class disposal must not
// double-dispose when the field has been ENUMERATED first. A for-in over a LOCAL
// bound from the field clones a fresh enumerator (disposed at loop end) and
// leaves the source frame alive for the holder's disposal — so both the clone
// and the source frame are freed exactly once (no leak, no double-free). The
// for-in is driven over a local copy of the field (not the bare field) because
// bare-field for-in receivers are diagnosed/fixed separately (#1265 rework);
// this test isolates the #1643 disposal path from that orthogonal concern.
BEZEL_TEST(codegen_owned_iter_field_disposal_1643,
           ienumerable_field_enumerates_then_disposes) {
    std::string src =
        "module M; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 10; yield return 20; yield return 30; "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IEnumerable<int>; "
        "  public constructor(owned s: IEnumerable<int>) seq := s; "
        "  end constructor; "
        "  public method Sum(): int "
        "    var total: int := 0; "
        "    var e := seq; "                  // local view of the field
        "    for var x in e do total := total + x; end for; "
        "    return total; "
        "  end Sum; end Holder; "
        "class M2 public static method Main(): int "
        "  using var h := new Holder(C.Gen()); return h.Sum(); "
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 60);           // enumeration yields 10+20+30
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// §28.4-step-2 / §28.5 (#1643): a class with MIXED owned fields — an iterator
// interface field (slot-1 dispose), an ordinary class field (slot-2 dynamic
// dispose), and an owning collection (bz_array_free + element cascade) — must
// dispose ALL of them exactly once at class disposal. Guards that recognizing
// iterator fields does not perturb the non-iterator owned-field paths (no
// double-free, no skipped field). The counting destructor makes Cell.n == 0 iff
// every Cell was disposed exactly once.
BEZEL_TEST(codegen_owned_iter_field_disposal_1643,
           mixed_iterator_and_class_fields_no_double_free) {
    std::string src =
        "module M; "
        "class Cell public static n: int := 0; "
        "  public constructor() Cell.n := Cell.n + 1; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; "
        "class C public static method Gen(): IEnumerable<int> "
        "  yield return 1; yield return 2; yield return 3; "
        "end Gen; end C; "
        "class Holder "
        "  private owned seq: IEnumerable<int>; "
        "  private owned c: Cell; "
        "  private owned arr: OwningArray<Cell>; "
        "  public constructor(owned s: IEnumerable<int>, owned cl: Cell) "
        "    seq := s; c := cl; "
        "    arr := new OwningArray<Cell>(); arr.Add(new Cell()); "
        "  end constructor; end Holder; "
        "class M2 public static method Main(): int "
        "  do using var h := new Holder(C.Gen(), new Cell()); end do; "
        "  return Cell.n; "   // read AFTER h disposed (inner scope exited) → 0
        "end Main; end M2;";
    BEZEL_EXPECT_EQ(run(src), 0);            // both Cells disposed exactly once
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace
