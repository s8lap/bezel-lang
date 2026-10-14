// Regression pin: the emitted OBJECT for a program is a pure function of that
// program, independent of what the hosting process compiled before it.
//
// WHY THIS MATTERS. The AOT harness (aot_support.hpp) compiles every program
// in a fork of the test binary, so the child inherits whatever compiler state
// the parent accumulated. Many codegen tests call `codegen::emitModule`
// IN-PROCESS in the parent, and the backend used to number its synthesized
// functions (`lam$N`, `thunk$N`, `gthunk$N`, `pthunk$N`, `objthunk$…$N`,
// `primthunk$…$N`, `spawn.lam$N`, `spawn.adapter$N`, `__bz_raiseasync_shim_N`,
// `async.lambda.N`) from function-local `static std::atomic` counters. Those
// survived across compilations, so the same program compiled after a different
// test history got different internal symbol names, a different symbol table
// and relocation order, and therefore a different object. That defeated the
// harness's object-content link-cache key (K2 = H(object ‖ runtime ‖ recipe)),
// and a compiler whose output depends on unrelated earlier inputs can hide
// order-dependent bugs. The counters are now per-backend-instance (one
// instance per compilation), restarting at 0 for every program.
//
// THE ORACLE. Compile program A to an object through the REAL harness path
// (forkedCompileObject -> Driver::compileExecutableObject, the path the K2 key
// hashes), then emit an unrelated lambda-heavy program B in THIS process (as
// the in-process codegen tests do), then compile A again. The two objects must
// be byte-identical. Neither object embeds the scratch path (A is compiled
// from the same source path both times regardless), so a byte comparison of
// the whole object is exact, not a normalised one.
//
// Fail-without: with the static counters restored, B's emission advances
// `lam$N` in the parent, the second fork of A starts numbering where B left
// off, and the objects differ. Pass-with: both forks start at 0.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;
namespace aot = bezel::test::aot_detail;

// Program A: non-capturing and capturing lambdas, so its object contains
// several backend-synthesized `lam$N` functions whose names come from the
// per-compilation counter.
const char* kProgramA =
    "module M; class C "
    "  public _base: int := 10; "
    "  public method Run(): int "
    "    var f: (int) -> int := lambda(x: int) => x * 2; "
    "    var g: (int) -> int := lambda(x: int) => x + this._base; "
    "    return f(3) + g(4); "
    "  end Run; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Run(); "
    "  end Main; "
    "end C;";

// Program B: unrelated, also lambda-heavy. Emitting it in the PARENT advanced
// the process-wide counters before the fix.
const char* kProgramB =
    "module N; class D "
    "  public static method Main(): int "
    "    var a: (int) -> int := lambda(x: int) => x + 1; "
    "    var b: (int) -> int := lambda(x: int) => x + 2; "
    "    var c: (int) -> int := lambda(x: int) => x + 3; "
    "    return a(1) + b(2) + c(3); "
    "  end Main; "
    "end D;";

// Lex + parse + sema + emitModule in THIS process, exactly as the in-process
// codegen tests do. Returns whether a module was produced.
bool emitInProcess(const char* source) {
    bezel::test::ensureNativeTargetInitialized();
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(source, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return false;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    if (diags.hasErrors()) return false;
    llvm::LLVMContext llvmCtx;
    auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx, {});
    return module != nullptr;
}

std::string slurpBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

}  // namespace

BEZEL_TEST(object_reproducibility,
           same_program_same_object_after_unrelated_in_process_emission) {
    const fs::path dir = bezel_test::wtScratchDir("objrepro");
    const fs::path src = dir / "a.bzl";
    {
        std::ofstream out(src, std::ios::binary);
        out << kProgramA;
    }
    const bezel::codegen::CodegenOptions opts;

    const fs::path obj1 = dir / "a1.o";
    const auto s1 = aot::forkedCompileObject(src.string(), obj1.string(), opts,
                                             (dir / "a1.err").string());
    BEZEL_EXPECT_TRUE(s1.outcome == aot::CompileOutcome::Ok);

    // The intervening, unrelated compilation in the parent process.
    BEZEL_EXPECT_TRUE(emitInProcess(kProgramB));

    const fs::path obj2 = dir / "a2.o";
    const auto s2 = aot::forkedCompileObject(src.string(), obj2.string(), opts,
                                             (dir / "a2.err").string());
    BEZEL_EXPECT_TRUE(s2.outcome == aot::CompileOutcome::Ok);

    const std::string b1 = slurpBytes(obj1);
    const std::string b2 = slurpBytes(obj2);
    BEZEL_EXPECT_FALSE(b1.empty());
    BEZEL_EXPECT_EQ(b1.size(), b2.size());
    BEZEL_EXPECT_TRUE(b1 == b2);
}

// CAUSE 2 — the object-emission TargetMachine carried state across emissions.
// LLVM's ELF object-file lowering records every `llvm.used` global of each
// module it emits in a set of GlobalObject POINTERS on the TargetMachine that is
// never cleared. emitObjectFile used to emit through ONE process-wide machine,
// so a later global that sat at a recorded address was emitted as if it were in
// `llvm.used` (SHF_GNU_RETAIN, its own section). In the harness that address
// was a freed global of an earlier, unrelated in-process emission, so which
// global caught it depended on allocation history.
//
// The pin makes the stale pointer DETERMINISTIC instead of allocation-dependent:
// emit a module whose global `g` is in `llvm.used`, then delete `llvm.used` (the
// global `g` itself stays alive at the same address) and emit the module again.
// That second object must equal the object of the same module built without
// `llvm.used` from the start. Fail-without: the reused machine still lists `g`,
// so the second object gives `g` a retained unique section and the bytes differ.
namespace {

std::unique_ptr<llvm::Module> makeRetainProbeModule(llvm::LLVMContext& ctx,
                                                    bool withUsed) {
    auto m = std::make_unique<llvm::Module>("retain_probe", ctx);
    auto* i32 = llvm::Type::getInt32Ty(ctx);
    auto* g = new llvm::GlobalVariable(
        *m, i32, /*isConstant=*/true, llvm::GlobalValue::ExternalLinkage,
        llvm::ConstantInt::get(i32, 7), "repro_probe_global");
    if (withUsed) {
        auto* ptrTy = llvm::PointerType::getUnqual(ctx);
        auto* arrTy = llvm::ArrayType::get(ptrTy, 1);
        auto* used = new llvm::GlobalVariable(
            *m, arrTy, /*isConstant=*/false,
            llvm::GlobalValue::AppendingLinkage,
            llvm::ConstantArray::get(arrTy, {g}), "llvm.used");
        used->setSection("llvm.metadata");
    }
    return m;
}

// Emits WITHOUT ASan instrumentation, deliberately and regardless of
// BEZEL_AOT_FORCE_SANITIZE (#3917): nothing here is ever executed — the claim
// is about the emitted BYTES — and the AddressSanitizer pass rewrites the
// module IN PLACE (new globals, its own used-lists), so instrumenting the
// first emission of `m` below would change the module the second emission
// sees and the comparison would no longer isolate the machine's state.
bool emitUninstrumented(llvm::Module& m, const fs::path& path) {
    std::string err;
    return bezel::codegen::emitObjectFile(m, path.string(), err,
                                          /*sanitizeAddress=*/false);
}

}  // namespace

BEZEL_TEST(object_reproducibility,
           object_emission_does_not_inherit_a_previous_modules_used_set) {
    bezel::test::ensureNativeTargetInitialized();
    const fs::path dir = bezel_test::wtScratchDir("objrepro_tm");

    // Reference: the module never had `llvm.used`.
    llvm::LLVMContext refCtx;
    auto ref = makeRetainProbeModule(refCtx, /*withUsed=*/false);
    BEZEL_EXPECT_TRUE(emitUninstrumented(*ref, dir / "ref.o"));

    // Same module, first emitted WITH `llvm.used`, then without it.
    llvm::LLVMContext ctx;
    auto m = makeRetainProbeModule(ctx, /*withUsed=*/true);
    BEZEL_EXPECT_TRUE(emitUninstrumented(*m, dir / "used.o"));
    m->getGlobalVariable("llvm.used")->eraseFromParent();
    BEZEL_EXPECT_TRUE(emitUninstrumented(*m, dir / "after.o"));

    const std::string refBytes = slurpBytes(dir / "ref.o");
    BEZEL_EXPECT_FALSE(refBytes.empty());
    BEZEL_EXPECT_TRUE(slurpBytes(dir / "after.o") == refBytes);
    // Control: `llvm.used` really does change the object, so the comparison
    // above can see a stale entry.
    BEZEL_EXPECT_TRUE(slurpBytes(dir / "used.o") != refBytes);
}
