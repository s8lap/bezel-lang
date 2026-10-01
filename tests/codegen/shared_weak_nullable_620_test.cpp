// Codegen tests for Shared<T>/Weak<T> as declared types in annotations —
// nullable locals/fields/params, Shared<T> params, Weak<T>? params, and
// OwningArray<Shared<T>> iteration (issue #620 / spec §28.13 / stdlib §2.16).
//
// Root cause (#620): "Shared" and "Weak" were absent from lowerTypeRef's
// builtin-name lists and from isBuiltinTypeRefName().  This caused:
//   (a) `Shared<T>? := null` → {i8,i32} struct lowering (bare-type-param path)
//       instead of ptrTy() → LLVM checkGEPType assert crash.
//   (b) `Shared<T>` parameter type → i32 lowering → bz_shared_get(i32) IR fail.
//   (c) `for var item in OwningArray<Shared<T>>` → i32 iteration variable
//       type → bz_shared_get(i32) IR fail.
//
// Test groups:
//   1. Shared<T>? nullable local — = null, <> null, null-check, ?., !
//   2. Shared<T> parameter — borrowed + field read via Get()
//   3. Weak<T>? nullable local — = null, <> null
//   4. OwningArray<Shared<T>> iteration — values verified by running
//   5. Shared<T> owned field in a class — disposed via cascade, LSan clean
//
// Spec refs (v0.135):
//   §28.13 "Shared ownership: Shared<T> and Weak<T>"
//   stdlib §2.16 "Shared<T> and Weak<T>"

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

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

#include "codegen/LlvmBackend.hpp"
#include "support/unique_stem.hpp"

namespace fs = std::filesystem;
using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

// Compile + link with ASan/LSan, run, return 0 (clean), 1 (sanitizer), -1 (skip).
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_620_lsan");
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
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectLeakFree(const std::string& source, int expectedExit) {
    BEZEL_EXPECT_EQ(run(source), expectedExit);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Inner class used across groups.
const char* kInner =
    "module M; "
    "class Inner "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  destructor Inner.disposed := Inner.disposed + 1; end destructor; "
    "end Inner; ";

} // anonymous namespace


// ─────────────────────────────────────────────────────────────────────────────
// Group 1: Shared<T>? nullable local.
// §28.13 / §7.1: a Shared<T>? annotation must lower to a plain pointer (ptr),
// not a {i8,i32} value-struct.  Before fix #620 this caused an LLVM
// checkGEPType assertion crash on `= null`, `<> null`, `?.`, `!`.
// ─────────────────────────────────────────────────────────────────────────────

// §28.13 / §7.1 (#620): `var s: Shared<Inner>? := null` lowers the annotation
// correctly; `if s = null` does not crash — previously a SIGSEGV from an LLVM
// assert because the nullable path produced {i8,i32} instead of ptr.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_nullable_local_assign_null) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    var s: Shared<Inner>? := null; "
        "    if s = null then "
        "      return 0; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.13 / §7.1 (#620): `<> null` on a Shared<T>? local.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_nullable_local_neq_null) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "    var s: Shared<Inner>? := null; "
        "    if s <> null then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.13 / §7.1 (#620): `?.` optional-access on a non-null Shared<T>? reads
// inner field via Get().  This required Shared<T>? to lower to ptr so the `?.`
// deref compiles to a pointer-null-check, not a struct tag check.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_nullable_local_optional_access) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 55; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    return h.Get().V; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 55);
}

// §28.13 / §7.1 (#620): `!` force-unwrap on a non-null Shared<T>?
// (annotation-typed local).  Confirms the lowering is ptr, not value-struct.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_nullable_local_force_unwrap) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method TryGet(s: Shared<Inner>?): int "
        "    if s = null then return -1; end; "
        "    return 0; "
        "  end TryGet; "
        "  public static method Main(): int "
        "    var s: Shared<Inner>? := null; "
        "    return C.TryGet(s); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), -1);
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 2: Shared<T> as a method parameter type.
// §28.13 / stdlib §2.16 (#620): a method parameter declared `h: Shared<Inner>`
// must lower to ptrTy(), not i32.  Before fix #620 the i32 lowering caused
// `bz_shared_get(i32 %X)` — a pointer expected, i32 provided — IR-verify fail.
// ─────────────────────────────────────────────────────────────────────────────

// §28.13 (#620): Shared<T> parameter — borrow, call Get(), read field.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_shared_param_get_field) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method ReadV(h: Shared<Inner>): int "
        "    var b := h.Get(); "
        "    return b.V; "
        "  end ReadV; "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 42; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    return C.ReadV(h); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 42);
}

// §28.13 (#620): Shared<T> parameter — multiple calls verify the value.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_shared_param_multiple_reads) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method SumV(h: Shared<Inner>, times: int): int "
        "    var acc := 0; "
        "    var i := 0; "
        "    while i < times do "
        "      acc := acc + h.Get().V; "
        "      i := i + 1; "
        "    end; "
        "    return acc; "
        "  end SumV; "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 3; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    return C.SumV(h, 4); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 12);
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 3: Weak<T>? nullable local.
// §28.13 / stdlib §2.16 (#620): Weak<T>? must also lower to ptrTy().
// ─────────────────────────────────────────────────────────────────────────────

// §28.13 (#620): `var w: Weak<Inner>? := null` — null-check does not crash.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_weak_nullable_local_null) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    var w: Weak<Inner>? := null; "
        "    if w = null then "
        "      return 0; "
        "    end; "
        "    return 1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 0);
}

// §28.13 (#620): Weak<T>? neq-null check.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_weak_nullable_local_nonnull) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    var obj := new Inner(); "
        "    obj.V := 7; "
        "    using var h := new Shared<Inner>(owned obj); "
        "    using var w := h.Downgrade(); "
        "    if w.TryUpgrade(out using var h2) then "
        "      return h2.Get().V; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 7);
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 4: OwningArray<Shared<T>> iteration.
// §28.13 / stdlib §4.5 (#620): iterating over OwningArray<Shared<T>> must
// produce a Shared<T> loop variable typed as ptrTy(), so Get() call compiles
// as bz_shared_get(ptr), not bz_shared_get(i32).  Values verified by running.
// ─────────────────────────────────────────────────────────────────────────────

// §28.13 / §4.5 (#620): iterate OwningArray<Shared<Inner>>, call Get(), sum V.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_owned_array_iteration_get) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<Shared<Inner>>(); "
        "    var o1 := new Inner(); o1.V := 10; "
        "    using var h1 := new Shared<Inner>(owned o1); "
        "    arr.Add(h1.Clone()); "
        "    var o2 := new Inner(); o2.V := 20; "
        "    using var h2 := new Shared<Inner>(owned o2); "
        "    arr.Add(h2.Clone()); "
        "    var sum := 0; "
        "    for var item in arr do "
        "      sum := sum + item.Get().V; "
        "    end; "
        "    return sum; "
        "  end Main; "
        "end C;";
    // 10 + 20 = 30
    BEZEL_EXPECT_EQ(run(src), 30);
}

// §28.13 / §4.5 (#620): OwningArray<Shared<Inner>> disposes all handles when
// the array is disposed → inner objects disposed; LSan clean.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_owned_array_disposes_handles_lsan) {
    std::string src = std::string(kInner) +
        "class C "
        "  public static method Fill() "
        "    using var arr := new OwningArray<Shared<Inner>>(); "
        "    var o1 := new Inner(); "
        "    using var h1 := new Shared<Inner>(owned o1); "
        "    arr.Add(h1.Clone()); "
        "    var o2 := new Inner(); "
        "    using var h2 := new Shared<Inner>(owned o2); "
        "    arr.Add(h2.Clone()); "
        "  end Fill; "
        "  public static method Main(): int "
        "    C.Fill(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    // Both inner objects disposed when arr + the original handles are released.
    expectLeakFree(src, 2);
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 5: Shared<T> as an owned field in a class.
// §28.13 / §28.10 (#620): a class with a Shared<T> owned field must dispose
// the handle when the class instance is disposed (disposal cascade).
// ─────────────────────────────────────────────────────────────────────────────

// §28.13 / §28.10 (#620): Shared<Inner> owned field — holder disposal releases
// the handle → inner disposed → LSan clean.
BEZEL_TEST(codegen_shared_weak_nullable_620, spec_28_13_shared_field_disposed_with_holder) {
    std::string src = std::string(kInner) +
        "class Holder "
        "  private owned _handle: Shared<Inner>; "
        "  public constructor(owned h: Shared<Inner>) "
        "    _handle := h; "
        "  end constructor; "
        "end Holder; "
        "class C "
        "  public static method MakeAndRelease() "
        "    var obj := new Inner(); "
        "    using var h := new Shared<Inner>(owned obj); "
        "    using var holder := new Holder(h.Clone()); "
        "  end MakeAndRelease; "
        "  public static method Main(): int "
        "    C.MakeAndRelease(); "
        "    return Inner.disposed; "
        "  end Main; "
        "end C;";
    // When MakeAndRelease returns: holder (its _handle clone) is released first
    // (scope-end reverse order) → strong count 2→1; then h (original) is
    // released → strong count 1→0 → Inner disposed exactly once.
    expectLeakFree(src, 1);
}
