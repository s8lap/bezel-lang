// Codegen + runtime — a `FromCollectionLiteral` USER TYPE as the target of a
// collection literal at the four positional positions (issue #2957, spec
// §15.4.1, v0.240). §15.4 admits "a user-defined type declaring the
// `FromCollectionLiteral` static factory" (§15.4.1) to its sequence-literal
// target list; the sema gate added by #2914 (Wave 4 commits 6b32328 +
// 7c49be6) explicitly opts in to FCL user types. The codegen side had no
// arm for that target: every `using var x: Bag := [e1, e2, e3];` for a
// user class `Bag` declaring `public static method FromCollectionLiteral(…):
// owned Bag` reached the inline-array GEP path on a bare-`ptr` slot and
// emitted `getelementptr ptr, ptr %0, i32 0, i32 0` — the LLVM verifier
// rejected it ("Invalid indices for GEP pointer type!") and the build
// aborted. The field-init / return / call-arg arms had the same defect
// (the same fallback, the same malformed GEP) for the same reason.
//
// The fix adds materializeUserTypeCollectionLiteral:
//   * BORROWING form (`ReadOnlyArraySpan<U>`): materializes a heap-allocated
//     bz_array_t<U> as the span's backing storage (the indexer reads via
//     bz_array_len / bz_array_get, so the source MUST be a real
//     bz_array_t* — not a raw element buffer), fills it with the n element
//     values via bz_array_add, wraps it in a ReadOnlyArraySpan<U> value, emits
//     the static call, and releases the backing via bz_array_free
//     IMMEDIATELY after the call (the factory must not retain the span
//     beyond its own execution per §15.4.1 "Lowering" Borrowing form).
//   * OWNING form (`owned OwningArray<U>`): delegates to
//     materializeHeapCollectionLiteral on a synthesized OwningArray<U>
//     TypeRef (so the disposer-stamp + partial-fill-throw unwind cleanup
//     the #2771 / #2635 spec-correct arm owns applies unchanged); the
//     `owned` parameter transfers ownership into the callee (no caller
//     disposal — no double-free, §28.3).
//
// These tests verify (1) local-decl position builds and returns the right
// sum, (2) field-init position initializes correctly, (3) return position
// builds and returns the right sum, (4) call-argument position (Bag
// parameter) builds and returns the right sum, (5) the empty-literal case
// is admitted (per §15.4.1 last paragraph) and reaches the factory's
// empty-case branch, (6) the owning form builds and disposes exactly once
// across a loop (ASan dispose-once), (7) the borrowing form is
// ASan-clean across a loop (no double-free of the backing bz_array_t),
// and (8) the owning-form spec-example shape `Cell(10)+Cell(20)+Cell(30)=60`
// is materialized through the `runAot` pipeline (issue #3029, post-#3027
// parser-fix unblock — the disabled sub-test
// `owning_form_class_elem_materializes_spec_15_4_1` from
// `tests/codegen/from_collection_literal_local_decl_2957_test.cpp`).
//
// narrow #3857 (test-only, §15.4.1, v0.272): `kBagClass`'s borrowing carrier
// was renamed `ReadOnlySpan<int>` → `ReadOnlyArraySpan<int>` by proposal
// 0115; the old spelling is no longer recognized (rejected at the
// declaration as of the same fix), so every borrowing-form cell below would
// fail to compile instead of exercising the value it names. Updated in the
// same commit as the recognition fix. `T` stays `int` throughout this file
// (value-typed), so none of these cells exercise #3857's D4 companion fix
// (the fresh-producer rejection for class-typed T) — see
// `collection_literal_borrow_form_readonlyarrayspan_3857_test.cpp`.

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
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir2957() {
    return wtScratchDir("wt-2957");
}

// Compile + run natively; return exit code or -1 on failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir2957();
    std::string stem = unique_stem("bezel_2957");
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
    auto dir = wtDir2957();
    std::string stem = unique_stem("bezel_2957_san");
    obj = (dir / (stem + ".o")).string();
    exe = (dir / stem).string();
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return false;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return false;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return false;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, obj, err)) return false;
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
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// Assert correct exit code AND LSan-clean (no leak, no UAF/double-free).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ---- Programs --------------------------------------------------------------

// §15.4.1: a user type with a public static `FromCollectionLiteral` factory,
// BORROWING form (`ReadOnlyArraySpan<int>`). The factory returns `new Bag(sum)`
// where sum is the three-element sum. Used at all four positional arms
// (local-decl, field-init, return, call-arg).
const std::string kBagClass =
    "class Bag "
    "  public Count: int; "
    "  public static method FromCollectionLiteral("
    "      items: ReadOnlyArraySpan<int>): owned Bag "
    "    if items.Length = 0 then return new Bag(99); end; "
    "    return new Bag(items[0] + items[1] + items[2]); "
    "  end FromCollectionLiteral; "
    "  public constructor(n: int) Count := n; end constructor; "
    "end Bag; ";

// §15.4.1: LOCAL-DECL — `using var x: Bag := [4, 5, 6];` is the issue's repro
// shape. Pre-fix the codegen emitted a malformed 2-index GEP into a bare
// `ptr` slot and the LLVM verifier aborted (issue #2957's measured
// failure); with the fix the static factory call produces an `owned Bag`
// whose Count is 15 (4 + 5 + 6).
const std::string kLocalDeclBorrowing =
    std::string("module M; ") + kBagClass +
    "class C "
    "  public static method Main(): int "
    "    using var bg: Bag := [4, 5, 6]; "
    "    return bg.Count; "
    "  end Main; "
    "end C;";

// §15.4.1: FIELD-INIT — `public B: Bag := [10, 20, 30];` is the field-init
// shape. The literal in a field initializer previously went through the
// same malformed-GEP path; with the fix the field holds an owned Bag
// whose Count is 60 (10 + 20 + 30).
const std::string kFieldInitBorrowing =
    std::string("module M; ") + kBagClass +
    "class Holder public B: Bag := [10, 20, 30]; end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h: Holder := new Holder(); "
    "    return h.B.Count; "
    "  end Main; "
    "end C;";

// §15.4.1: RETURN — `return [7, 8, 9];` from a static method whose
// declared return type is `owned Bag` materializes a literal and returns
// the factory's result. Pre-fix the return path's heap materializer
// returned null for a user type (same root cause as the local-decl arm)
// and the return degraded to zero; with the fix the returned Bag's
// Count is 24 (7 + 8 + 9).
const std::string kReturnBorrowing =
    std::string("module M; ") + kBagClass +
    "class C "
    "  public static method Make(): owned Bag return [7, 8, 9]; end Make; "
    "  public static method Main(): int "
    "    using var m: Bag := Make(); "
    "    return m.Count; "
    "  end Main; "
    "end C;";

// §15.4.1: CALL-ARGUMENT — `Consume([4, 5, 6])` where Consume's parameter is
// `b: Bag`. The literal at the call-argument position targets the
// parameter's Bag type — the FCL opt-in is the parameter type, not the
// call's enclosing class. With the fix the materialized Bag's Count is
// 15 (4 + 5 + 6).
const std::string kCallArgBorrowing =
    std::string("module M; ") + kBagClass +
    "class C "
    "  public static method Consume(b: Bag): int return b.Count; end Consume; "
    "  public static method Main(): int return Consume([4, 5, 6]); end Main; "
    "end C;";

// §15.4.1 last paragraph: an empty literal `[]` is admitted at a FCL user
// type — the factory must handle the empty case (the spec gives it as the
// factory's responsibility). The factory above returns 99 in the empty
// branch; the local-decl position exercises it.
const std::string kEmptyLiteralBorrowing =
    std::string("module M; ") + kBagClass +
    "class C "
    "  public static method Main(): int "
    "    using var e: Bag := []; "
    "    return e.Count; "
    "  end Main; "
    "end C;";

// §15.4.1 OWNING form: a user type with `public static method
// FromCollectionLiteral(elements: owned OwningArray<Cell>): owned Bag`.
// The owning form builds an `owned OwningArray<Cell>` via
// materializeHeapCollectionLiteral (so the disposer-stamp + partial-fill-
// throw unwind cleanup the #2771 / #2635 spec-correct arm owns applies
// unchanged); the `owned` parameter transfers ownership into the callee.
// Count is the sum of the three cells' V values.
const std::string kBagOwningClass =
    "class Cell "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Cell; "
    "class Bag "
    "  public Count: int; "
    "  public static method FromCollectionLiteral("
    "      elements: owned OwningArray<Cell>): owned Bag "
    "    if elements.Count = 0 then return new Bag(77); end; "
    "    var total: int := 0; "
    "    for var i in 0..<elements.Count do "
    "      total := total + elements[i].V; "
    "    end; "
    "    return new Bag(total); "
    "  end FromCollectionLiteral; "
    "  public constructor(n: int) Count := n; end constructor; "
    "end Bag; ";

const std::string kLocalDeclOwning =
    std::string("module M; ") + kBagOwningClass +
    "class C "
    "  public static method Main(): int "
    "    using var bg: Bag := [new Cell(4), new Cell(5), new Cell(6)]; "
    "    return bg.Count; "
    "  end Main; "
    "end C;";

// §15.4.1 OWNING form re-enable shape (issue #3029 — the spec-example
// values `new Cell(10), new Cell(20), new Cell(30)`, sum = 60). The original
// disabled test lived in
// `tests/codegen/from_collection_literal_local_decl_2957_test.cpp` (deleted
// in the #2957 revert, 49bdfb1); this is the same shape, asserted via
// `runAot` per the brief.
const std::string kLocalDeclOwningReenabled =
    std::string("module M; ") + kBagOwningClass +
    "class C "
    "  public static method Main(): int "
    "    using var bg: Bag := [new Cell(10), new Cell(20), new Cell(30)]; "
    "    return bg.Count; "
    "  end Main; "
    "end C;";

// §15.4.1 / §28.3: BORROWING form looped — the per-call backing bz_array_t is
// released immediately after each FCL call returns, so a 1000-iteration
// loop is ASan-clean (no double-free of the backing, no leak of the per-
// iteration backing storage). Pre-fix the path didn't exist; post-fix this
// pins the backing-free semantics.
const std::string kBorrowingLoopLeakFree =
    std::string("module M; ") + kBagClass +
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var bg: Bag := [1, 2, 3]; "
    "      t := t + bg.Count; "
    "      i := i + 1; "
    "    end; "
    "    if t = 6000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

// §15.4.1 / §28.3: OWNING form looped — the per-call `owned OwningArray<Cell>`
// transfers into the callee and the callee disposes it. A 1000-iteration
// loop is ASan-clean (no double-free, no leak). Pre-fix the path didn't
// exist; post-fix this pins the ownership-transfer semantics.
const std::string kOwningLoopLeakFree =
    std::string("module M; ") + kBagOwningClass +
    "class C "
    "  public static method Main(): int "
    "    var i: int := 0; var t: int := 0; "
    "    while i < 1000 do "
    "      using var bg: Bag := [new Cell(1), new Cell(2), new Cell(3)]; "
    "      t := t + bg.Count; "
    "      i := i + 1; "
    "    end; "
    "    if t = 6000 then return 0; end; "
    "    return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ---- Value correctness (the #2957 ICE cases) -------------------------------

// §15.4.1: LOCAL-DECL position — `using var x: Bag := [4, 5, 6];` builds and
// Count is 15 (4 + 5 + 6). Pre-fix: malformed-GEP ICE.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           local_decl_borrowing_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kLocalDeclBorrowing), 15);
}

// §15.4.1: FIELD-INIT position — `public B: Bag := [10, 20, 30];` builds and
// Count is 60 (10 + 20 + 30). Pre-fix: malformed-GEP ICE on the field
// initializer.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           field_init_borrowing_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kFieldInitBorrowing), 60);
}

// §15.4.1: RETURN position — `return [7, 8, 9];` from a method returning
// `owned Bag` builds and Count is 24 (7 + 8 + 9). Pre-fix: degraded to a
// zero return (#2491).
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           return_borrowing_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kReturnBorrowing), 24);
}

// §15.4.1: CALL-ARGUMENT position — `Consume([4, 5, 6])` where Consume
// takes `b: Bag` builds and Count is 15 (4 + 5 + 6). Pre-fix: same
// malformed-GEP ICE as the local-decl arm.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           call_arg_borrowing_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kCallArgBorrowing), 15);
}

// §15.4.1 last paragraph: EMPTY literal `[]` at a FCL user type is
// admitted and reaches the factory's empty-case branch (Count is 99 in
// this test's factory). Pre-fix: not exercised (the malformed GEP ICE'd
// on any literal); post-fix: the borrowing form's empty path builds a
// Count-0 bz_array_t as the backing and the indexer's bounds check
// accepts length 0.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           empty_literal_borrowing_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kEmptyLiteralBorrowing), 99);
}

// §15.4.1 OWNING form: LOCAL-DECL position — `using var x: Bag := [new
// Cell(4), new Cell(5), new Cell(6)];` builds and Count is 15. Pre-fix:
// path didn't exist.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           local_decl_owning_count_15_4_1) {
    BEZEL_EXPECT_EQ(compileAndRun(kLocalDeclOwning), 15);
}

// §15.4.1 OWNING form re-enable (issue #3029, post-#3027 parser-fix
// unblock): LOCAL-DECL position with the spec-example class-typed elements
// `[new Cell(10), new Cell(20), new Cell(30)]` — the `owned OwningArray<Cell>`
// aggregate is materialized, ownership transfers into the `FromCollectionLiteral`
// call (§28.3), the factory drains it (sum is 60), and the temp aggregate is
// discharged by the callee. This is the re-enable of the disabled sub-test
// `owning_form_class_elem_materializes_spec_15_4_1` originally in
// `tests/codegen/from_collection_literal_local_decl_2957_test.cpp`
// (disabled in 0edf69e pending the parser fix; that file was deleted in the
// #2957 revert, 49bdfb1, and replaced with this one — same arm, different
// shape). The assertion shape uses `runAot` so the test exercises the full
// AOT pipeline (fork-isolated `Driver::compileFile` + clang link + native
// exec) per CLAUDE.md's strict-oracle rule.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           owning_form_class_elem_materializes_spec_15_4_1) {
    JitResult r = runAot(kLocalDeclOwningReenabled);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 60);
}

// ---- ASan dispose-exactly-once (no leak, no double-free) --------------------

// §15.4.1 / §28.3: BORROWING form looped — per-call backing released
// after each FCL call returns; LSan-clean across 1000 iterations.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           borrowing_loop_leak_free_28_3) {
    expectValueAndLeakFree(kBorrowingLoopLeakFree, 0);
}

// §15.4.1 / §28.3: OWNING form looped — ownership transfers into the
// callee, callee disposes once; LSan-clean across 1000 iterations.
BEZEL_TEST(codegen_collection_literal_user_type_fcl_2957,
           owning_loop_leak_free_28_3) {
    expectValueAndLeakFree(kOwningLoopLeakFree, 0);
}
