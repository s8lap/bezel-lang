// Codegen — §28.4 / §28.5: auto-disposal of an `owned` field whose declared
// type is a generic type PARAMETER (`private owned _slot: T`), when the generic
// class is instantiated with a class.
//
// §28.4 step 2: the compiler-generated Dispose disposes all owned-bearing
// fields in reverse declaration order, calling the field value's Dispose().
// §28.5 "Generic class fields": when a generic declares `private owned _slot: T`
// (T a type parameter), the class-or-interface restriction is checked at
// INSTANTIATION; instantiating with a class auto-disposes the field.
//
// THE BUG (FINDING [1]): the disposer is one shared, type-erased function per
// generic class, emitted with no active substitution; it classified the field
// by its DECLARED type (a bare type parameter T → not a class/interface) and so
// never disposed it. Box<Res> leaked its inner Res (destructor count stayed 0).
//
// THE FIX: a generic class with an owned type-parameter field gets a PER-
// INSTANTIATION specialized vtable whose slot-1 Dispose classifies the field by
// the CONCRETE substituted type of that instantiation (Res → a class → dispose;
// int → a value → leave). The disposal itself is the ordinary dynamic dispatch
// (the field object's vtable slot 1) + reclaim, fired exactly once.
//
// Two checks per program (mirroring box_refcount_test.cpp): a native exit code
// proves the destructor ran the right number of times, and (when the toolchain
// is present) LeakSanitizer proves the inner object is reclaimed exactly once —
// no leak (the bug) and no double-free / use-after-free.

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
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_gowned_" + std::to_string(counter++);
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

std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path()             // tests/codegen
                        .parent_path()             // tests
                        .parent_path();            // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// LeakSanitizer verification for a single program. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem (leak or corruption): test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_gowned_lsan_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
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
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }

    // exitcode=99: a DISTINCT ASan failure code (no test expects 99), so a
    // leak/UAF/double-free can never be confused with the program's own exit.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert correctness (exit code == destructor count) AND, when the toolchain is
// present, that LSan reports no leak / UAF / double-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// --- Programs ------------------------------------------------------------

// FINDING [1] / §28.5: Box<T> with `private owned _value: T`, T = Res (a class
// with a counting destructor). Disposing the Box (the `using var` scope end)
// must dispose the inner Res EXACTLY ONCE → Res.count == 1. Before the fix the
// disposer never touched the type-parameter field → count 0 + a 16-byte leak.
const char* kBoxOfClassDisposedOnce =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Box<T> "
    "  private owned _value: T; "
    "  public constructor(owned v: T) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var b := new Box<Res>(new Res()); "
    "    end; "
    "    return Res.count; "                  // 1: inner Res disposed exactly once
    "  end Main; "
    "end C;";

// §28.4 step 2: TWO owned type-parameter fields are disposed in REVERSE
// declaration order. Each Res carries a distinct id; its destructor appends that
// id to a shared base-10 sequence accumulator (seq := seq*10 + id). _b (declared
// second, id 2) disposes first → seq 2; _a (declared first, id 1) second → seq
// 2*10 + 1 = 21. The value 21 (rather than 12) is what proves REVERSE order; it
// also proves both fields were disposed. (Reading the moved-out construction
// arguments would be a §28.3 use-after-move, so disposal order is observed
// through statics, not through the now-owned locals.)
const char* kTwoOwnedTypeParamFieldsReverseOrder =
    "module M; "
    "class Res "
    "  public static seq: int := 0; "
    "  public id: int; "
    "  public constructor(i: int) id := i; end constructor; "
    "  destructor Res.seq := Res.seq * 10 + id; end destructor; "
    "end Res; "
    "class Pair<T> "
    "  private owned _a: T; "
    "  private owned _b: T; "
    "  public constructor(owned x: T, owned y: T) "
    "    _a := x; _b := y; "
    "  end constructor; "
    "  destructor end destructor; "
    "end Pair; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var p := new Pair<Res>(new Res(1), new Res(2)); "
    "    end; "
    "    return Res.seq; "                    // 21: _b (id 2) disposed before _a (id 1)
    "  end Main; "
    "end C;";

// §28.4 / §28.5: a generic class with BOTH an owned type-parameter field (T)
// and a CONCRETE owned class field (Res). Disposing the owner disposes BOTH,
// each exactly once → count == 2. Guards that the per-instantiation specialized
// disposer disposes the concrete field too (it re-emits ALL field disposals for
// the instantiation, not only the type-parameter one).
const char* kMixedTypeParamAndConcreteOwnedField =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Holder<T> "
    "  private owned _t: T; "
    "  private owned _r: Res; "
    "  public constructor(owned v: T) _t := v; _r := new Res(); end constructor; "
    "  destructor end destructor; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var h := new Holder<Res>(new Res()); "
    "    end; "
    "    return Res.count; "                  // 2: the T field AND the Res field
    "  end Main; "
    "end C;";

// §28.5 instantiation-time classification: instantiating Box<T> with a VALUE
// type (int) where `owned` would not apply must NOT dispose the field as a
// reference — no spurious dispatch on the value slot, no crash. (The compiler
// does not yet reject this at instantiation per §28.5; this test pins that
// whatever it does, the value instantiation runs cleanly and disposes nothing.)
// Returns 7; LSan-clean (an int is value-typed — nothing to leak or free).
const char* kBoxOfValueTypeNoSpuriousDisposal =
    "module M; "
    "class Box<T> "
    "  private owned _value: T; "
    "  public constructor(owned v: T) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var b := new Box<int>(5); "
    "    end; "
    "    return 7; "
    "  end Main; "
    "end C;";

// Two DISTINCT instantiations of the same generic in one program: Box<Res>
// (class → field disposed) and Box<int> (value → field not disposed). Each must
// get its own specialized disposer; the class one disposes once (count == 1),
// the value one does nothing and does not crash. Returns Res.count == 1.
const char* kTwoInstantiationsSameGeneric =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Box<T> "
    "  private owned _value: T; "
    "  public constructor(owned v: T) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var a := new Box<Res>(new Res()); "
    "      using var b := new Box<int>(5); "
    "    end; "
    "    return Res.count; "                  // 1: only the Box<Res> field disposed
    "  end Main; "
    "end C;";

// CONTROL (no regression): the NON-generic owned-class-field case still disposes
// exactly once. Identical shape to kBoxOfClassDisposedOnce but with the field
// typed concretely as Res rather than a parameter T. Returns 1.
const char* kNonGenericControlDisposedOnce =
    "module M; "
    "class Res "
    "  public static count: int := 0; "
    "  public constructor() end constructor; "
    "  destructor Res.count := Res.count + 1; end destructor; "
    "end Res; "
    "class Box "
    "  private owned _value: Res; "
    "  public constructor(owned v: Res) _value := v; end constructor; "
    "  destructor end destructor; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    do "
    "      using var b := new Box(new Res()); "
    "    end; "
    "    return Res.count; "                  // 1
    "  end Main; "
    "end C;";

}  // namespace

// --- Tests ---------------------------------------------------------------

BEZEL_TEST(codegen_generic_owned_field, box_of_class_disposed_once_28_5) {
    // FINDING [1]: the inner Res of a Box<Res> is disposed exactly once and its
    // 16 bytes reclaimed. Leaked (count 0) before the fix; clean (count 1) after.
    expectValueAndLeakFree(kBoxOfClassDisposedOnce, 1);
}

BEZEL_TEST(codegen_generic_owned_field, two_owned_type_param_fields_reverse_order_28_4) {
    // §28.4 step 2: two owned type-parameter fields are disposed in reverse
    // declaration order (_b before _a) → 21, and both freed (LSan-clean).
    expectValueAndLeakFree(kTwoOwnedTypeParamFieldsReverseOrder, 21);
}

BEZEL_TEST(codegen_generic_owned_field, mixed_type_param_and_concrete_owned_field_28_4) {
    // A generic class's owned T field AND its concrete owned Res field are both
    // disposed exactly once by the specialized disposer → count 2, LSan-clean.
    expectValueAndLeakFree(kMixedTypeParamAndConcreteOwnedField, 2);
}

BEZEL_TEST(codegen_generic_owned_field, box_of_value_type_no_spurious_disposal_28_5) {
    // §28.5: a value-type instantiation does not dispose the field as a
    // reference — no crash, nothing leaked or double-freed.
    expectValueAndLeakFree(kBoxOfValueTypeNoSpuriousDisposal, 7);
}

BEZEL_TEST(codegen_generic_owned_field, two_instantiations_same_generic_28_5) {
    // Box<Res> and Box<int> in one program: distinct specialized disposers; the
    // class field disposed once, the value field untouched → count 1.
    expectValueAndLeakFree(kTwoInstantiationsSameGeneric, 1);
}

BEZEL_TEST(codegen_generic_owned_field, non_generic_control_disposed_once_28_5) {
    // CONTROL: the non-generic owned-class-field path is unchanged (no
    // regression) — disposed exactly once, LSan-clean.
    expectValueAndLeakFree(kNonGenericControlDisposedOnce, 1);
}
