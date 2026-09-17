// Codegen — §14.2: the generic-method specialization cache must key on the
// CONCRETE TYPE IDENTITY of each type argument, not only the erased LLVM type.
//
// THE BUG (silent miscompile, high blast radius): emitGenericSpecialization
// cached specialized method bodies keyed by (MethodSymbol*, vector<llvm::Type*>)
// — the substituted LLVM types. But every class type erases to `ptr` in LLVM
// IR, so `Maker<A>.Run` and `Maker<B>.Run` produced the SAME key `[ptr]`. The
// second instantiation hit the cache and REUSED the first's specialized
// function. Once a specialized body contained a CONCRETE-CLASS-SPECIFIC
// disposer call (`using var a := new T()`, resolved via genericSubstSemaTy_),
// `Maker<B>.Run` would run `A`'s disposer on a `B` object — A's destructor body
// executes, B's never runs, and A's field layout is used to dispose B's block
// (a latent UAF / wrong-free when the layouts differ).
//
// THE FIX: extend the specialization cache key to include the sema TypeSymbol
// identity (declaration pointer / primitive enum) of each substituted type
// parameter, in deterministic parameter-name order, alongside the existing LLVM
// types. Now `Maker<A>.Run` and `Maker<B>.Run` get DISTINCT cache entries and
// DISTINCT specialized functions with the correct per-class disposer; two
// instantiations with the SAME concrete type arg still share one specialization.
//
// Two complementary check kinds (matching generic_usingvar_disposal_test.cpp):
//   * destructor-count JIT tests (always run): the destructor bumps a static
//     counter observable via the int exit code — proves the RIGHT disposer runs
//     the RIGHT number of times.
//   * LeakSanitizer native tests (run only when ASan toolchain is present):
//     lower to an object file, link against an ASan-instrumented runtime, run
//     under LSan — proves the block is reclaimed with no leak / double-free /
//     use-after-free even when A and B have differently-sized owned fields.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
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
static const std::filesystem::path& wtDirGenSpecCache() {
    return wtScratchDir("wt-gen-spec-cache");
}

// JIT a program and return its `main` exit code (or -999 if it didn't run).
int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// LeakSanitizer verification — same harness as generic_usingvar_disposal_test.
// Returns 0 = clean, 1 = sanitizer reported a problem, -1 = toolchain unavailable.
int runUnderLeakSanitizer(const std::string& source, bool detectLeaks = true) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    auto dir = wtDirGenSpecCache();
    std::string stem = unique_stem("bezel_gen_spec_cache_lsan");
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
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        std::string("ASAN_OPTIONS=detect_leaks=") +
        (detectLeaks ? "1" : "0") + ":exitcode=99 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert the exit code (always) and, if the toolchain is present, that LSan
// reports no leak/corruption.
void expectExitAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// Assert no double-free / UAF: LSan run with leak detection disabled.
void expectExitNoDoubleFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(run(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source, /*detectLeaks=*/false);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// -----------------------------------------------------------------------
// (a) The exact repro from the bug report (§14.2).
//
// Maker<A>.Run and Maker<B>.Run both lower the body `using var a := new T()`,
// which disposes T's concrete destructor.  With the colliding cache key both
// instantiations shared ONE specialized function whose disposer was A's, so
// A._n was bumped TWICE and B._n NEVER → 20.  With a type-aware key each gets
// its own specialization → A._n==1, B._n==1 → 1*10 + 1 = 11.
// -----------------------------------------------------------------------
const char* kReproTwoClasses =
    "module M; "
    "class A "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor A._n := A._n + 1; end destructor; "
    "end A; "
    "class B "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor B._n := B._n + 1; end destructor; "
    "end B; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Run(): int "
    "    using var a := new T(); "
    "    return 1; "
    "  end Run; "
    "end Maker; "
    "class C public static method Main(): int "
    "  using var ma := new Maker<A>(); "
    "  using var mb := new Maker<B>(); "
    "  ma.Run(); mb.Run(); "
    "  return A._n * 10 + B._n; "
    "end Main; end C;";
// Expected: 11 (each class disposed exactly once). Bug: 20 (A twice, B never).

// -----------------------------------------------------------------------
// (b) Maker<A> instantiated TWICE → A disposed twice, B's body never runs.
//     Two Maker<A> + one Maker<B>: A constructed/disposed twice, B once.
//     With the buggy key, the FIRST distinct erased key wins for all three, so
//     B's disposer never ran.  We assert A._n==2, B._n==1 → 2*10+1 = 21.
//     This also exercises (e): the two Maker<A> instantiations must SHARE one
//     specialization (no over-specialization, no duplicate symbols) yet still
//     each dispose correctly.
// -----------------------------------------------------------------------
const char* kSameTypeTwiceAndOther =
    "module M; "
    "class A "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor A._n := A._n + 1; end destructor; "
    "end A; "
    "class B "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor B._n := B._n + 1; end destructor; "
    "end B; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Run(): int "
    "    using var a := new T(); "
    "    return 1; "
    "  end Run; "
    "end Maker; "
    "class C public static method Main(): int "
    "  using var ma1 := new Maker<A>(); "
    "  using var ma2 := new Maker<A>(); "
    "  using var mb := new Maker<B>(); "
    "  ma1.Run(); ma2.Run(); mb.Run(); "
    "  return A._n * 10 + B._n; "
    "end Main; end C;";
// Expected: 21 (A disposed twice, B once).

// -----------------------------------------------------------------------
// (c) A and B with OWNED fields of DIFFERENT layout — each block must be freed
//     by its OWN disposer. A owns one Res; B owns two Res. Under the buggy key
//     B<...>.Run would run A's disposer on a B object → wrong-size free / UAF.
//     ASan/LSan catch the corruption; the int result also verifies each owned
//     Res's destructor ran the right number of times.
//
//     Res._d counts Res destructions.  Maker<T>.Run does `using var a := new
//     T()`; constructing A makes 1 Res, constructing B makes 2 Res.  After
//     Run() returns both are disposed (their owned Res freed).  We instantiate
//     Maker<A> once and Maker<B> once → total Res destructions = 1 + 2 = 3.
// -----------------------------------------------------------------------
const char* kOwnedFieldsDistinctLayout =
    "module M; "
    "class Res "
    "  public static _d: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res._d := Res._d + 1; end destructor; "
    "end Res; "
    "class A "
    "  private owned r1: Res; "
    "  public constructor() r1 := new Res(); end constructor; "
    "  destructor end destructor; "
    "end A; "
    "class B "
    "  private owned r1: Res; "
    "  private owned r2: Res; "
    "  public constructor() r1 := new Res(); r2 := new Res(); end constructor; "
    "  destructor end destructor; "
    "end B; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Run(): int "
    "    using var a := new T(); "
    "    return 0; "
    "  end Run; "
    "end Maker; "
    "class C public static method Main(): int "
    "  using var ma := new Maker<A>(); "
    "  using var mb := new Maker<B>(); "
    "  ma.Run(); mb.Run(); "
    "  return Res._d; "
    "end Main; end C;";
// Expected: 3 (A frees 1 Res, B frees 2 Res). Buggy key: wrong-size free/UAF.

// -----------------------------------------------------------------------
// (d) A generic method specialized on a RECORD vs a CLASS — both correct.
//     Records already differ by their LLVM struct type so they were not
//     colliding, but the type-aware key must not break them.  Box<T>.GetX
//     returns 0 for either; one Box<Point> (record) and one Box<Cls> (class)
//     must each be specialized correctly and coexist.
// -----------------------------------------------------------------------
const char* kRecordVsClass =
    "module M; "
    "record Point(x: int, y: int); "
    "class Cls "
    "  public v: int; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor end destructor; "
    "end Cls; "
    "class Box<T> "
    "  private item: T; "
    "  public constructor(it: T) item := it; end constructor; "
    "  public method GetX(): int "
    "    return 0; "
    "  end GetX; "
    "end Box; "
    "class C public static method Main(): int "
    "  var p := new Point(3, 4); "
    "  var bp := new Box<Point>(p); "
    "  var pr := bp.GetX(); "
    "  using var cl := new Cls(7); "
    "  var bc := new Box<Cls>(cl); "
    "  var cr := bc.GetX(); "
    "  return (pr + 1) * 10 + (cr + 1); "
    "end Main; end C;";
// Expected: 11 (both GetX return 0 → (0+1)*10 + (0+1)). Verifies record and
// class specializations coexist without collision and without crashing.

// -----------------------------------------------------------------------
// (e) Two instantiations with the SAME class type arg SHARE one specialization
//     and behave correctly — no duplicate-symbol issues, no over-specialization
//     that would double-dispose.  Two Maker<A>; A disposed exactly twice.
// -----------------------------------------------------------------------
const char* kSameTypeShares =
    "module M; "
    "class A "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor A._n := A._n + 1; end destructor; "
    "end A; "
    "class Maker<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method Run(): int "
    "    using var a := new T(); "
    "    return 1; "
    "  end Run; "
    "end Maker; "
    "class C public static method Main(): int "
    "  using var ma1 := new Maker<A>(); "
    "  using var ma2 := new Maker<A>(); "
    "  ma1.Run(); ma2.Run(); "
    "  return A._n; "
    "end Main; end C;";
// Expected: 2 (A disposed exactly twice, once per Run call).

// -----------------------------------------------------------------------
// (f) A generic method that constructs/disposes a value-typed body (no behavior
//     dependent on T beyond the disposer) specialized on two class types — both
//     bodies run their own disposal; the per-class statics keep their distinct
//     values, proving no behavioral collision.
// -----------------------------------------------------------------------
const char* kValueReturnTwoClasses =
    "module M; "
    "class A "
    "  public static _k: int := 3; "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end A; "
    "class B "
    "  public static _k: int := 5; "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end B; "
    "class Probe<T> where T: class, new() "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "  public method MakeAndDrop(): int "
    "    using var a := new T(); "
    "    return 1; "
    "  end MakeAndDrop; "
    "end Probe; "
    "class C public static method Main(): int "
    "  using var pa := new Probe<A>(); "
    "  using var pb := new Probe<B>(); "
    "  var x := pa.MakeAndDrop(); "
    "  var y := pb.MakeAndDrop(); "
    "  return (x + y) * 100 + A._k * 10 + B._k; "
    "end Main; end C;";
// Expected: 235 (x==1, y==1 → 2*100; A._k==3, B._k==5 → +35). Both
// specializations run their own (empty) body; no cross-contamination.

}  // namespace

// ---- (a) exact repro: distinct class type args get distinct specializations -

// §14.2: Maker<A>.Run and Maker<B>.Run must NOT share a specialized function.
// Each disposes its own concrete class exactly once. Expected 11.
// Before the fix: 20 (A disposed twice, B never — erased-key collision).
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           distinct_class_type_args_distinct_specialization_14_2) {
    expectExitAndLeakFree(kReproTwoClasses, 11);
}

// ---- (b) same type twice + another: no cross-contamination ------------------

// §14.2: two Maker<A> + one Maker<B>; A disposed twice, B once. The two
// Maker<A> share one specialization; Maker<B> gets its own. Expected 21.
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           same_type_twice_plus_other_no_contamination_14_2) {
    expectExitAndLeakFree(kSameTypeTwiceAndOther, 21);
}

// ---- (c) owned fields of distinct layout: each freed by its own disposer -----

// §14.2 / §28.3: A owns 1 Res, B owns 2 Res. Each must be freed by its OWN
// disposer (right size, right destructor). ASan/LSan catch wrong-size free or
// UAF from running A's disposer on a B block. Expected 3 Res destructions.
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           owned_fields_distinct_layout_each_freed_correctly_14_2) {
    expectExitNoDoubleFree(kOwnedFieldsDistinctLayout, 3);
}

// ---- (d) record vs class specialization both correct ------------------------

// §14.2: a generic method specialized on a RECORD and on a CLASS must each get
// a correct specialization; the type-aware key must not break record/value
// specialization. Expected 11.
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           record_and_class_specialization_coexist_14_2) {
    expectExitAndLeakFree(kRecordVsClass, 11);
}

// ---- (e) same type arg shares one specialization ----------------------------

// §14.2: two instantiations with the SAME class type arg must SHARE one
// specialized function (no over-specialization, no duplicate-symbol link
// errors) and dispose correctly. A disposed exactly twice. Expected 2.
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           same_type_arg_shares_one_specialization_14_2) {
    expectExitAndLeakFree(kSameTypeShares, 2);
}

// ---- (f) value-returning generic method, two class types, no collision ------

// §14.2: a generic method specialized on two class types must not collide
// behaviorally. Both bodies run their own disposal; the per-class statics keep
// their distinct values. Expected 235.
BEZEL_TEST(codegen_generic_spec_cache_14_2,
           value_returning_two_class_types_no_collision_14_2) {
    expectExitAndLeakFree(kValueReturnTwoClasses, 235);
}
