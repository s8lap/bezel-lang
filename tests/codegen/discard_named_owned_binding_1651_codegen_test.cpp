// fix #1651 / §17.13 / §28.3 (v0.184): codegen-observable coverage for
// "Discard of a named owned binding" — `_ := x;` on an owned local (any decl
// form) or an `owned` parameter is an ANNOUNCED CONSUME, disposed at the END
// OF THE DISCARD STATEMENT, not at scope end.
//
// The sema-level diagnostics (no more must-use auto-drop warning, the §28.2
// use-after-discard warning, the maybe-consumed join error, the borrow-state
// carve-out) are covered in
// tests/ownership/discard_named_owned_binding_1651_test.cpp. This file pins
// the two things only codegen execution can prove:
//
//   (a) TIMING: the destructor genuinely runs at the discard statement, not
//       merely at scope end (a static-counter capture placed immediately
//       after the discard sees the post-dispose count).
//   (b) NO DOUBLE-FREE / NO LEAK: the ownership pass's markDisposedByName +
//       the reused discardOutOwnedVar codegen path (explicitDisposeOwnedReceiverSlot,
//       which nulls the receiver's slot) correctly suppress the LATER
//       scope-end / using-var / owned-parameter teardown disposal for the
//       same binding — verified both by a static counter staying at exactly
//       one disposal and, where the sanitizer toolchain is available, by
//       LeakSanitizer (no leak, no double-free, no use-after-free).
//
// Every test class here has NO instance-level refcounted (string/object)
// field — the disposal counter is a STATIC int — so the disposer-path reclaim
// (emitDisposeAndReclaim) is exact for these types and the LSan legs are
// clean. (A separate, pre-existing codegen gap — the disposer/reclaim path
// does not release a class's plain non-owned string/object *instance* fields;
// it affects scope-end auto-drop, `.Dispose()`, and discards uniformly, and
// is orthogonal to #1651 — is deliberately not exercised here.)
//
// The `runOwn` / `runUnderLeakSanitizer` harness below mirrors (duplicated,
// per the existing convention) tests/codegen/undisposed_leak_test.cpp's
// #1009 harness: the shared `runJit` test helper (support/codegen_support.hpp)
// deliberately skips ownership::checkOwnership, so any ownership-pass-
// dependent codegen flag (here, DiscardStmt::discardOutOwnedVar) needs the
// full front end run explicitly.

#include <cstdio>
#include <filesystem>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1651() {
    return wtScratchDir("wt-1651");
}

// JIT a program with the FULL front-end INCLUDING the ownership pass (the
// shared runJit test helper does not run it — see file header). Returns
// `main`'s exit code, or -999 if the program did not compile/run.
int runOwn(const std::string& src) {
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

// LeakSanitizer verification: lowers `source` through the full front end
// (incl. the ownership pass), links against an ASan-instrumented runtime,
// runs under LSan. Returns 0 (clean), 1 (sanitizer failure — test failure),
// or -1 (toolchain unavailable — SKIP). Mirrors
// tests/codegen/undisposed_leak_test.cpp's runUnderLeakSanitizer.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir1651();
    std::string stem = unique_stem("bezel_1651_lsan");
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
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err)) return -1;
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
        std::string("ASAN_OPTIONS=detect_leaks=1:exitcode=99 ") + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer flagged
    return 0;                                                      // clean
}

// Every dtor increments the shared STATIC counter (a disposal count). No
// instance-level refcounted field — so emitDisposeAndReclaim is exact for R.
const char* kR =
    "class R public static _n: int := 0; "
    "  destructor _n := _n + 1; end destructor; end R; ";

}  // namespace

// ===========================================================================
// (a) TIMING, FAIL-WITHOUT / PASS-WITH: `mid` is captured IMMEDIATELY after
// the discard statement, before any scope-end cleanup could run. Before the
// fix `x` was untouched by the discard (state stayed Owned) — R's
// destructor ran only as part of Main's own return-path/scope-end cleanup,
// which happens AFTER `mid := R._n;` already captured the value — so `mid`
// was 0 (not yet disposed). With the fix, the discard disposes `x`
// immediately: `mid` is 1.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651_codegen, plain_local_disposed_before_next_statement) {
    const char* src_tmpl =
        "module M; %s"
        "class C public static method Main(): int "
        "  var x := new R(); "
        "  _ := x; "
        "  var mid := R._n; "   // captured right after the discard
        "  return mid; "
        "end Main; end C;";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), src_tmpl, kR);
    BEZEL_EXPECT_EQ(runOwn(buf), 1);
}

// ===========================================================================
// (b) NO DOUBLE-DISPOSE at scope end + LSan clean: the discard happens inside
// a helper; Main reads the counter after the helper (and its own scope
// teardown) has fully returned. markDisposedByName + the discardOutOwnedVar
// codegen path (explicitDisposeOwnedReceiverSlot) null the slot, so the
// plain-var scope-end auto-drop for the same binding is suppressed — disposed
// exactly once. The LSan leg is the sound double-free/leak proof.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651_codegen, plain_local_disposed_exactly_once_across_call) {
    const char* src_tmpl =
        "module M; %s"
        "class C "
        "  public static method Consume() "
        "    var x := new R(); "
        "    _ := x; "
        "  end Consume; "
        "  public static method Main(): int "
        "    Consume(); "
        "    return R._n; "
        "  end Main; "
        "end C;";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), src_tmpl, kR);
    std::string src(buf);
    BEZEL_EXPECT_EQ(runOwn(src), 1);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// `using var` receiver (v0.184's "Who may call Dispose()" list, spec:6036,
// names `using var` alongside a plain owned local/parameter as an eligible
// discard/dispose receiver): the discard disposes at the discard statement;
// the `using var`'s own (null-guarded) scope-end disposal then finds the
// slot null and is a no-op. Exactly one disposal, no double-free. This is
// the dedicated #1651 counterpart of the corrected #1009 comment in
// tests/codegen/undisposed_leak_test.cpp
// (discard_owned_local_alias_not_double_freed_1009), whose original v0.150
// premise ("a using-var alias discard is a no-op borrow read") this fix
// supersedes for exactly this reason.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651_codegen, using_var_disposed_at_discard_not_double_freed) {
    const char* src_tmpl =
        "module M; %s"
        "class C public static method Main(): int "
        "  using var x := new R(); "
        "  _ := x; "
        "  var mid := R._n; "
        "  return mid; "
        "end Main; end C;";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), src_tmpl, kR);
    std::string src(buf);
    BEZEL_EXPECT_EQ(runOwn(src), 1);   // disposed at the discard, not at scope end
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);  // and not disposed AGAIN at scope end
}

// ===========================================================================
// `owned` PARAMETER receiver: before the fix the generalized branch's
// predecessor gated on `dynamic_cast<sema::LocalSymbol*>` — a
// ParameterSymbol never matched, so an `owned` parameter discard was
// unhandled exactly like the plain-local case (disposed only at the
// method's own owned-parameter teardown, not at the discard). Fixed: the
// parameter is disposed at the discard statement and not disposed again by
// the method's normal-exit owned-parameter teardown.
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651_codegen, owned_parameter_disposed_at_discard_not_double_freed) {
    const char* src_tmpl =
        "module M; %s"
        "class C "
        "  public static method Consume(owned r: R): int "
        "    _ := r; "
        "    return R._n; "     // captured right after the discard, inside Consume
        "  end Consume; "
        "  public static method Main(): int "
        // Consume(new R()) — a FRESH producer needs no `owned` call-site
        // marker (only a named owned binding does, §22.3); the marker would
        // be spurious here.
        "    var mid := Consume(new R()); "
        "    var final := R._n; "
        "    return mid * 10 + final; "
        "  end Main; "
        "end C;";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), src_tmpl, kR);
    std::string src(buf);
    // mid=1: disposed at the discard, inside Consume, before Consume returns.
    // final=1: Consume's own normal-exit owned-parameter teardown does NOT
    // dispose `r` again (it was already left explicitly-disposed) — no
    // double-free.
    BEZEL_EXPECT_EQ(runOwn(src), 11);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// CONTROL: a BORROW parameter's discard disposes nothing — the destructor
// never runs inside the callee; the caller's own `using var` disposes the
// value exactly once, after the call returns. No leak, no double-free
// either way (this program's soundness is unaffected by #1651 — included as
// the borrow-state control for the LSan leg).
// ===========================================================================
BEZEL_TEST(discard_named_owned_binding_1651_codegen, borrow_parameter_discard_no_dispose_control) {
    const char* src_tmpl =
        "module M; %s"
        "class C "
        "  public static method Touch(b: R): int "
        "    _ := b; "
        "    return R._n; "   // captured INSIDE the callee: must still be 0
        "  end Touch; "
        "  public static method Main(): int "
        "    using var owner := new R(); "
        "    return Touch(owner); "
        "  end Main; "
        "end C;";
    char buf[1024];
    std::snprintf(buf, sizeof(buf), src_tmpl, kR);
    std::string src(buf);
    BEZEL_EXPECT_EQ(runOwn(src), 0);  // not yet disposed inside Touch
    int lsan = runUnderLeakSanitizer(src);
    if (lsan >= 0) BEZEL_EXPECT_EQ(lsan, 0);
}
