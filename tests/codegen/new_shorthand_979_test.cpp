// Codegen + sema + ownership — #979 / §17.7: the `new(args)` shorthand (the
// construction target inferred from the LHS annotation) must construct the
// object and run its constructor, exactly like the explicit `new Type(args)`.
//
// Root cause (pre-fix): the shorthand parses to a NewExpr with a null `type`.
// Sema's NewExpr typing (`if (ne->type)`) was skipped, so the node was never
// typed; codegen's lowerNew (`if (!ne.type) return nullptr`) skipped
// construction and the uninitialized local was dereferenced → SIGSEGV. For a
// record target the ownership pass's producerState also defaulted a typeless
// `new` to Owned, so a value-type `new Record(...)` shorthand was wrongly
// rejected as an undisposed owned resource (§28.3).
//
// The fix: sema records the declared type as the shorthand NewExpr's exprType
// (and validates the constructor args via checkNewExprArgs); lowerNew and the
// ownership producerState both read that exprType when `ne.type` is null.
//
// The record case's fix lives in the ownership pass, which the shared runJit
// helper does NOT run — so this uses a full-pipeline runner (sema + ownership +
// codegen + JIT), mirroring the #945 test.

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
int run(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    bezel::test::ensureNativeTargetInitialized();
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return -999;
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return -999; }
    auto jit = std::move(*jitOrErr);
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    return sym->toPtr<int (*)()>()();
}
bool rejectedAtCompile(const std::string& src) {
    return bezel::test::parseAndCheckOwnership(src).diagnostics.hasErrors();
}
const char* kConfig =
    "class Config public V: bool; "
    "  public constructor(verbose: bool) V := verbose; end constructor; "
    "  destructor end destructor; end Config; ";
}  // namespace

// §17.7 / #979: class shorthand with a NAMED argument — the named ctor arg is
// bound and the object constructed (before the fix this SIGSEGV'd).
BEZEL_TEST(new_shorthand_979, class_named_arg_constructs) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kConfig +
        "class C public static method Main(): int "
        "  using var cfg: Config := new(verbose := true); "
        "  return (1 if cfg.V else 0); "
        "end Main; end C;"), 1);
}

// §17.7 / #979: class shorthand with a POSITIONAL argument.
BEZEL_TEST(new_shorthand_979, class_positional_arg_constructs) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kConfig +
        "class C public static method Main(): int "
        "  using var cfg: Config := new(true); "
        "  return (1 if cfg.V else 0); "
        "end Main; end C;"), 1);
}

// §17.7 / #979: no-arg class shorthand runs the parameterless constructor.
BEZEL_TEST(new_shorthand_979, class_no_arg_runs_ctor) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Box public V: int; "
        "  public constructor() V := 7; end constructor; "
        "  destructor end destructor; end Box; "
        "class C public static method Main(): int "
        "  using var b: Box := new(); "
        "  return b.V; "
        "end Main; end C;"), 7);
}

// §17.7 / #979: RECORD shorthand — a value type, so the ownership pass must
// accept it (not reject as an undisposed owned resource) AND it must construct
// with the positional arguments.
BEZEL_TEST(new_shorthand_979, record_shorthand_is_value_and_constructs) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pt(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p: Pt := new(3, 4); "
        "  return p.x + p.y; "
        "end Main; end C;"), 7);
}

// §17.7 / #979: a wrong-typed shorthand argument is rejected at compile time
// (the ctor-arg validation runs for the shorthand, exactly as for `new Type`).
BEZEL_TEST(new_shorthand_979, wrong_arg_rejected) {
    BEZEL_EXPECT_TRUE(rejectedAtCompile(
        std::string("module M; ") + kConfig +
        "class C public static method Main(): int "
        "  using var cfg: Config := new(123); "
        "  return 0; "
        "end Main; end C;"));
}
