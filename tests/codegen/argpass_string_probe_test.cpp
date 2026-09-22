// Probe matrix for #429 ARGUMENT-PASS retain category (§28.9 / §4.6).
// Measures which shapes leak BEFORE the fix is implemented.
// Run with detect_leaks=1 → exit 99 means leak, exit 0 means clean.
// This file is PROBE-ONLY: it records the current (pre-fix) behavior.
// The real tests live in argpass_string_refcount_test.cpp.

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
static const std::filesystem::path& wtDirArgpassProbe() {
    return wtScratchDir("wt-argpass-probe");
}

// Compile + link against ASan runtime, run under LSan. Returns:
//   0  = clean (no leak)
//   1  = sanitizer problem (leak or corruption)
//  -1  = toolchain unavailable / compile failure (SKIP)
int probeLsan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;
    auto dir = wtDirArgpassProbe();
    std::string stem = unique_stem("bezel_argpass_probe");
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
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        std::remove(objPath.c_str());
        return -3;
    }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(objPath.c_str());
    std::remove(exe.c_str());
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // leak/corruption
    return 0;  // clean
}

// Compile + run natively; return exit code or -1 on failure.
int probeRun(const std::string& source) {
    auto dir = wtDirArgpassProbe();
    std::string stem = unique_stem("bezel_argpass_run");
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

// Helper: assert correctness (exit code) AND record the LSan result.
// Records are printed to stderr for the human-readable probe matrix.
void probeShape(const char* label, const std::string& source,
                int expectedExitCode) {
    int run = probeRun(source);
    BEZEL_EXPECT_EQ(run, expectedExitCode);
    int lsan = probeLsan(source);
    if (lsan == -1) {
        std::fprintf(stderr, "  %-60s  SKIP (no toolchain)\n", label);
    } else {
        std::fprintf(stderr, "  %-60s  %s\n", label,
                     lsan == 0 ? "CLEAN" : "LEAK");
    }
}

// ---- Shape S1: concat passed directly as arg, callee only reads it ----------
// The callee `Consume` just returns `s.Length` — no store, pure borrow.
// The concat "he"+"llo" produces rc=1; nobody releases it → LEAK.
const char* kS1ConcatDirectArg =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(\"he\" + \"llo\"); "  // concat → rc=1, leaked
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S2: user-method return string passed directly as arg -------------
// `Build()` returns a +1 string; it's passed directly to `Consume` (borrow).
// The +1 from Build's return is leaked.
const char* kS2MethodReturnDirectArg =
    "module M; class C "
    "  public static method Build(): string return \"bu\" + \"ilt\"; end Build; "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(C.Build()); "  // +1 from Build leaked
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S3: string interpolation passed directly as arg ------------------
// `$"x={n}"` produces rc=1; passed to `Consume` as borrow → leaked.
const char* kS3InterpDirectArg =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume($\"x=3\"); "  // interp → rc=1, leaked
    "      i := i + 1; "
    "    end; "
    "    if total = 300000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S4: multiple producer args in same call --------------------------
// Both args are +1 producers; each leaks independently.
const char* kS4MultipleProducerArgs =
    "module M; class C "
    "  public static method Combine(a: string, b: string): int "
    "    return a.Length + b.Length; "
    "  end Combine; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Combine(\"aa\" + \"bb\", \"cc\" + \"dd\"); "  // two leaks
    "      i := i + 1; "
    "    end; "
    "    if total = 800000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S5: concat arg to a method that stores into a field --------------
// (Double-free canary!) The callee stores `s` into `this.Name` via field-store.
// Field-store of a BORROW param: retains (since param is NOT a +1 producer
// inside the callee). So rc=2 after store. Our post-call release → rc=1 (safe).
// This MUST NOT double-free, even after the fix is applied.
const char* kS5ConcatArgFieldStore =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\"; end constructor; "
    "  public method Set(s: string) this.Name := s; end Set; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    h.Set(\"ne\" + \"w\"); "  // concat → field-store retains → rc=2
    "    total := total + h.Name.Length; "  // "new" => 3
    "    i := i + 1; "
    "  end; "
    "  if total = 300000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Shape S6: literal string as arg (sentinel, no heap alloc) --------------
// A literal is a global (sentinel rc). Release is a no-op. No leak, no DF.
// This validates that our fix doesn't harm the literal case.
const char* kS6LiteralArg =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(\"lit\"); "  // sentinel — no heap alloc
    "      i := i + 1; "
    "    end; "
    "    if total = 300000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S7: local string (borrow, NOT a +1 producer at call site) --------
// `var s := "a" + "b"` → s owns +1. When s is passed as arg, it is a borrow
// (an identifier, not a +1 producer at call site). The caller's local s holds
// its +1 and releases at scope end (non-escaping local). No extra release.
// Validates that the fix doesn't double-release local variables.
const char* kS7LocalStringArg =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      var s: string := \"ab\" + \"cd\"; "  // producer local, owns +1
    "      total := total + C.Consume(s); "     // s is a borrow, NOT a producer
    "      i := i + 1; "                        // s released at scope end
    "    end; "
    "    if total = 400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S8: nested call foo(bar(s + "x")) --------------------------------
// Inner call arg `s + "x"` is a producer; `bar` treats it as borrow; then
// the result of bar (a +1 if bar is a user string-return method) is passed to
// foo. Both the inner (bar's arg) and outer (foo's arg) may leak.
const char* kS8NestedCallArg =
    "module M; class C "
    "  public static method Build(s: string): string return s + \"!\"; end Build; "
    "  public static method Len(s: string): int return s.Length; end Len; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Len(C.Build(\"ab\" + \"cd\")); "
    "      i := i + 1; "                        // "abcd!" => 5
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S9: constructor call with producer arg ---------------------------
// `new Holder("x" + "y")` — constructor receives a borrow param, stores into
// field (retains). Concat's +1 is leaked (the callee's field-store retains).
const char* kS9ConstructorProducerArg =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(\"ct\" + \"or\"); "  // concat leaked
    "    total := total + h.Name.Length; "  // "ctor" => 4
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Shape S10: method-return arg to method that stores into field ----------
// `Build()` returns +1; `h.Set(C.Build())` — callee stores param into field
// (retains). Build's +1 is leaked. Same canary as S5 but with method-return.
const char* kS10MethodReturnArgFieldStore =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\"; end constructor; "
    "  public method Set(s: string) this.Name := s; end Set; "
    "end Holder; "
    "class C "
    "  public static method Build(): string return \"bu\" + \"ilt\"; end Build; "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      h.Set(C.Build()); "  // Build returns +1, Set's field-store retains
    "      total := total + h.Name.Length; "  // "built" => 5
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Shape S11: discarded call statement with producer arg ------------------
// `C.SideEffect(s + "x")` used as a statement (result discarded). This is the
// most common case (e.g., Console.Write, logging). Producer leaked.
const char* kS11StatementLevelCallProducerArg =
    "module M; class C "
    "  public static method SideEffect(s: string): int return s.Length; end SideEffect; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 100000 do "
    "      _ := C.SideEffect(\"eff\" + \"ect\"); "  // concat leaked
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- Shape S12: producer arg to a virtual method call -----------------------
// Virtual/vtable dispatch: the concat's +1 is leaked after the virtual call.
const char* kS12VirtualCallProducerArg =
    "module M; class Base "
    "  public virtual method Consume(s: string): int return s.Length; end Consume; "
    "end Base; "
    "class Derived : Base "
    "  public override method Consume(s: string): int return s.Length + 1; end Consume; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    total := total + b.Consume(\"vt\" + \"ab\"); "  // concat leaked via vtable
    "    i := i + 1; "
    "  end; "
    "  if total = 500000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Shape S13: producer arg to interface method call -----------------------
const char* kS13ItableCallProducerArg =
    "module M; interface IConsumer "
    "  method Consume(s: string): int; "
    "end IConsumer; "
    "class Thing : IConsumer "
    "  public method Consume(s: string): int return s.Length; end Consume; "
    "end Thing; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var t := new Thing(); "
    "    var ic: IConsumer := t; "
    "    total := total + ic.Consume(\"it\" + \"ab\"); "  // concat "itab" len=4 → 400000
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Shape S14: slice arg passed directly -----------------------------------
// `src[0..3]` is a +1 producer (fresh allocation, "hell" len=4). Passed as arg → leaked.
const char* kS14SliceDirectArg =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var src: string := \"hello\"; "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(src[0..3]); "  // slice "hell" len=4 → 400000
    "      i := i + 1; "
    "    end; "
    "    if total = 400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ========================= PROBE TEST CASES ==================================
// These tests record the CURRENT (pre-fix) behavior.
// After the fix, the "LEAK" shapes should become "CLEAN".

BEZEL_TEST(codegen_argpass_probe, S1_concat_direct_arg_28_9) {
    probeShape("S1 concat direct arg (borrow-only callee)", kS1ConcatDirectArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S2_method_return_direct_arg_28_9) {
    probeShape("S2 user-method-return direct arg", kS2MethodReturnDirectArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S3_interp_direct_arg_28_9) {
    probeShape("S3 interpolation direct arg", kS3InterpDirectArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S4_multiple_producer_args_28_9) {
    probeShape("S4 multiple producer args in one call", kS4MultipleProducerArgs, 0);
}

BEZEL_TEST(codegen_argpass_probe, S5_concat_arg_field_store_canary_28_9) {
    // Double-free canary: callee stores via field-store (retains independently).
    // After the fix: release runs AFTER the store retain → rc=1 (safe).
    probeShape("S5 concat arg to field-store callee (canary)", kS5ConcatArgFieldStore, 0);
}

BEZEL_TEST(codegen_argpass_probe, S6_literal_arg_28_9) {
    probeShape("S6 literal arg (sentinel, no heap)", kS6LiteralArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S7_local_string_arg_28_9) {
    // Double-free canary: local string passed as arg — NOT a +1 producer.
    // The fix must NOT release locals (they have their own scope-end release).
    probeShape("S7 local string arg (NOT a producer)", kS7LocalStringArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S8_nested_call_arg_28_9) {
    probeShape("S8 nested call foo(bar(s+x))", kS8NestedCallArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S9_constructor_producer_arg_28_9) {
    probeShape("S9 constructor with producer arg (field-store retains)", kS9ConstructorProducerArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S10_method_return_field_store_canary_28_9) {
    probeShape("S10 method-return arg to field-store callee (canary)", kS10MethodReturnArgFieldStore, 0);
}

BEZEL_TEST(codegen_argpass_probe, S11_statement_level_call_producer_arg_28_9) {
    probeShape("S11 statement-level call with producer arg", kS11StatementLevelCallProducerArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S12_virtual_call_producer_arg_28_9) {
    probeShape("S12 virtual (vtable) call with producer arg", kS12VirtualCallProducerArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S13_itable_call_producer_arg_28_9) {
    probeShape("S13 interface (itable) call with producer arg", kS13ItableCallProducerArg, 0);
}

BEZEL_TEST(codegen_argpass_probe, S14_slice_direct_arg_28_9) {
    probeShape("S14 slice direct arg", kS14SliceDirectArg, 0);
}
