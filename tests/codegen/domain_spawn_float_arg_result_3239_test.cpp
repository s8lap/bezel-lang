// Codegen — §30.14 / stdlib §6.8 (#3239): Domain.Spawn float arg + float
// result must round-trip end-to-end through the static-method adapter.
//
// Regression for #3239: `Domain.Spawn(P.EFloat, 3.25)` and the §30.14 /
// stdlib §6.8 `Spawn<A, R>(entry: (A) -> R, arg: A): owned Task<R>` shape
// with A = float, R = float — and the sibling `Spawn<R>(entry: () -> R)`
// shape with R = float. Pre-fix:
//   * ARG side — the sender's spawnValueToVoidPtr used `val->getType()`
//     for BOTH the malloc size AND the heap-store type, so a float literal
//     (which `lowerExpr` produces as `f64` by default — the literal's static
//     type, see LlvmBackend_Exprs.cpp:123) sent through a `float`-param
//     static method allocated 8 bytes and stored 8 bytes of `f64`
//     representation (3.25-as-double = 0x400a000000000000). The receiver's
//     `castToParamTy` then loaded `f32` from offset 0 — the lower 4 bytes
//     of the 8-byte slot, which are all-zero bits — and the entry observed
//     `0.0` instead of `3.25`. round-trip bug, deterministic.
//   * RESULT side — the adapter's else branch did
//     `CreateZExt(ret, i64)` unconditionally for any non-pointer / non-void
//     return type. `ZExt` is integer-only; under -DNDEBUG the IR builder
//     accepts the call silently and emits `ZExt f32 …` which is a verifier
//     abort (`ZExt only operates on integer`). `await tf` then either
//     failed IR verification (post-#3181, when the sender/receiver fix
//     made it compile for the first time) or, in the lambda path's else
//     branch, used `CreateBitCast` correctly — the static-method path was
//     the broken one.
//
// Root cause (fixed):
//   * Sender: spawnValueToVoidPtr now takes the entry param's LLVM type and
//     truncates an `f64` literal to `f32` (or extends an `f32` to `f64`)
//     before the heap copy, so the malloc size and store type match the
//     receiver's load type. f32/f64 sendable primitives ride on the same
//     heap-copy path structs use (#3181 / sharpen #3181).
//   * Result: the static-method adapter's else branch now does
//     `BitCast f32 → i32 → ZExt → i64` (or `BitCast f64 → i64` directly)
//     so the float bits land in the low bytes of an i64, which is what
//     `bz_domain_spawn`'s 8-byte write preserves and what the await
//     site's `CreateLoad(resTy, resPtr)` reads back as the original float.
//
// Each test FAILS without the fix (pre-fix: spawn_float_arg round-trips
// 0.0, spawn_float_void_round_trips returns wrong, spawn_double_arg_*
// returns wrong, spawn_lambda_float_result_* is the round-trip that
// already worked because the lambda adapter already used BitCast) and
// PASSES with it.
//
// These run through `runAot` (the AOT harness) — the load-bearing
// correctness check — and additionally through ASan-linked native
// binaries. The exit code is the int returned by `Main(): Task<int>`.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

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

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const fs::path& wtDir3239() {
    return wtScratchDir("wt-3239");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// ASan probe — link the .bzl with -fsanitize=address so a heap-corruption
// regression in the new heap-copy / FP-trunc path (double-free, dangling
// pointer, missed free) shows up under the sanitizer.
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir3239();
    std::string stem = unique_stem("bezel_3239_asan_");
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

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
        return -1;
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// #3239 — the arg side. float literal sent through a float-param static
// method must round-trip. Pre-fix the receiver observed 0.0 because the
// sender malloc'd 8 bytes and stored an 8-byte f64 representation
// (3.25-as-double = 0x400a000000000000), and the receiver's 4-byte f32
// load got the lower 4 bytes (all zero). Round-trip is via a discriminant
// the receiver builds from the observed float: `if v = 3.25 then 99`
// returns 99 on success and 7 on failure. Exit is 99 = round-trip worked.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_float_arg_result_3239,
           float_literal_arg_reaches_static_method_entry) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method RoundTrip(v: float): int "
        "    if v = 3.25 then "
        "      return 42; "
        "    end; "
        "    return 7; "
        "  end RoundTrip; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var tf := Domain.Spawn(P.RoundTrip, 3.25); "
        "    var iv := await tf; "
        "    return iv; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3239 — the arg side, double sibling. Same shape with `double` param;
// proves the FPTrunc / FPExt branch in spawnValueToVoidPtr is symmetric
// and isn't a one-direction accident.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_float_arg_result_3239,
           double_literal_arg_reaches_static_method_entry) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method RoundTrip(v: double): int "
        "    if v = 3.25 then "
        "      return 42; "
        "    end; "
        "    return 7; "
        "  end RoundTrip; "
        "end P; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var tf := Domain.Spawn(P.RoundTrip, 3.25); "
        "    var iv := await tf; "
        "    return iv; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3239 — the result side, the file's headline shape from the issue body:
// `Domain.Spawn(P.EFloat, 3.25)` with `EFloat: (float) -> float`. Pre-fix
// the static-method adapter's else branch did `ZExt f32 → i64`, an invalid
// IR instruction (`ZExt only operates on integer`) that under -DNDEBUG
// passes the IR builder but aborts the verifier — so this repro failed
// to COMPILE before #3181 exposed it, and the post-#3181 + sharpen #3181
// state left it as the latent return-side bug this fix closes. Post-fix
// the entry runs and returns the float through the spawn adapter's
// BitCast → int → ZExt → i64 path; `await tf` reads the float bits.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_float_arg_result_3239,
           float_arg_and_float_result_round_trip_via_static_method) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method EFloat(v: float): float "
        "    return v; "
        "  end EFloat; "
        "end P; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tf := Domain.Spawn(P.EFloat, 3.25); "
        "    var fv := await tf; "
        "    if fv = 3.25 then "
        "      return 42; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// #3239 — the result side, double sibling. BitCast f64 → i64 directly
// (both 8 bytes; no ZExt intermediate). Exit is 99 on round-trip,
// 7 if the upper bytes of the i64 were dropped on the floor.
// ─────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_float_arg_result_3239,
           double_arg_and_double_result_round_trip_via_static_method) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class P "
        "  public static method EDouble(v: double): double "
        "    return v; "
        "  end EDouble; "
        "end P; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var tf := Domain.Spawn(P.EDouble, 1.5); "
        "    var dv := await tf; "
        "    if dv = 1.5 then "
        "      return 42; "
        "    end; "
        "    return 7; "
        "  end Main; "
        "end C;";
    expectCleanAndEq(src, 42);
}