// Codegen + runtime — string reference counting for the ARGUMENT-PASS category
// (issue #429, §4.6 / §28.9). When a +1 string producer (concat / interpolation
// / range-slice / user-method-return) is passed DIRECTLY as a call argument
// (not bound to a local first), the CALLER must release the temporary AFTER the
// call returns. The callee receives the string as a BORROW; if the callee stores
// the param into a string field/auto-property, the field-store path inside the
// callee already retains independently (rc→2), so the post-call release (rc→1)
// is safe — the field still owns its +1.  If the callee only reads the string,
// the post-call release (rc→0) reclaims the temporary.
//
// Ownership convention (argument-pass sub-slice):
//   - A +1 string producer in argument position is released by the CALLER after
//     the call returns (bz_string_release on the argument value).
//   - The release runs only on normal-exit paths; on exception paths the
//     temporary leaks (the safe direction — a leak is harmless, a UAF is not).
//   - Non-producer arguments (literals / local identifiers / field reads) are
//     NOT released (they do not hold an independent +1 from the call-site's
//     perspective). Literals carry the sentinel rc; locals' +1 is owned by the
//     local slot and released by the scope-end machinery.
//   - Double-free canaries: when the callee stores the argument into a field,
//     the field-store path retains (rc→2); our post-call release (rc→1) keeps
//     exactly one reference in the field. The canary programs exercise this path
//     under detect_leaks=0 / abort_on_error=0 (so we catch UAF/double-free while
//     tolerating any residual bookkeeping leak from closure-box cells).
//
// Two complementary checks per shape:
//   1. expectValueAndLeakFree: exit code correct AND LSan reports no leak /
//      UAF / double-free (the temporary is reclaimed → CLEAN).
//   2. expectValueAndNoCorruption: exit code correct AND ASan no MEMORY-SAFETY
//      error (UAF/double-free), tolerating a residual leak.  Used for field-store
//      canaries where a small unrelated residual may survive.

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
static const std::filesystem::path& wtDir429ap() {
    return wtScratchDir("wt-429ap");
}

// Compile + run natively; return exit code or -1 on failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429ap();
    std::string stem = unique_stem("bezel_429ap");
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

// Compile + link against ASan runtime. Returns false on failure / unavailable.
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir429ap();
    std::string stem = unique_stem("bezel_429ap_san");
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

// LSan verification. Returns 0 = clean, 1 = leak/corruption, -1 = skip.
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

// Assert correct exit code AND LSan-clean (no leak, no UAF/DF).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert correct exit code AND no ASan MEMORY-SAFETY error (UAF/DF), tolerating
// a residual leak (detect_leaks=0). Used for field-store canaries where a small
// unrelated residual may survive from pre-existing box/closure bookkeeping.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str()); std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ---- Programs: concat direct arg (borrow-only callee) -----------------------
//
// §28.9 / §4.6 / §28.3: a +1 producer (`+`) passed DIRECTLY as an argument to
// a method that only READS it (pure borrow). The producer's rc=1 reference was
// previously leaked (no one released it). With argument-pass retain, the caller
// emits bz_string_release AFTER the call → rc=0, freed. Looped 100k → memory
// bounded (no per-iteration growth). LSan-clean after the fix.

const char* kConcatDirectArgBorrowOnlyCallee =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(\"he\" + \"llo\"); "  // concat rc=1, released after call
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: user-method-return arg (borrow-only callee) ------------------
//
// §28.9 / §28.3 (RETURN × ARGUMENT-PASS interplay): `Build()` mints +1 for the
// returned string; the result is passed DIRECTLY to `Consume` (no local). The
// caller now releases the +1 after the outer call → reclaimed each iteration.
const char* kMethodReturnDirectArgBorrowOnlyCallee =
    "module M; class C "
    "  public static method Build(): string return \"bu\" + \"ilt\"; end Build; "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(C.Build()); "
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: string interpolation direct arg ------------------------------
//
// §28.9 / §4.6: an interpolation `$"…"` is a +1 producer. Passed as argument
// directly → released after the call.
const char* kInterpDirectArgBorrowOnlyCallee =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume($\"x=3\"); "  // interp → rc=1, released
    "      i := i + 1; "
    "    end; "
    "    if total = 300000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: multiple +1 producer args in one call -----------------------
//
// §28.9: both args are +1 producers; both are released after the call → bounded
// memory.
const char* kMultipleProducerArgsCall =
    "module M; class C "
    "  public static method Combine(a: string, b: string): int "
    "    return a.Length + b.Length; "
    "  end Combine; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Combine(\"aa\" + \"bb\", \"cc\" + \"dd\"); "
    "      i := i + 1; "
    "    end; "
    "    if total = 800000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: concat arg to field-store callee (double-free canary) --------
//
// §28.9 (CARDINAL double-free guard): the callee stores the param into a class
// string field via `this.Name := s`. The field-store path sees `s` (a param
// identifier, NOT a +1 producer at the callee's expression level) → RETAINS
// (rc=2). After the outer call returns, our post-call release drops rc to 1 —
// the field still holds its independent +1.  No double-free, no UAF.
// Looped 100k times; the field's final value is released on the object's
// disposal. Value + no-corruption (detect_leaks=0, tolerating any residual from
// object bookkeeping in the test harness).
const char* kConcatArgFieldStoreNoDoubleFree =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\"; end constructor; "
    "  public method Set(s: string) this.Name := s; end Set; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    h.Set(\"ne\" + \"w\"); "
    "    total := total + h.Name.Length; "  // "new" => 3
    "    i := i + 1; "
    "  end; "
    "  if total = 300000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs: literal arg (sentinel, no heap alloc) -----------------------
//
// §28.9: a literal string carries the sentinel rc — bz_string_release on a
// sentinel is a no-op. The fix emits a release on the literal value; the no-op
// ensures no spurious free. CONTROL: this was already CLEAN before the fix.
const char* kLiteralArgSentinelControl =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(\"lit\"); "
    "      i := i + 1; "
    "    end; "
    "    if total = 300000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: nested call foo(bar(s + "x")) --------------------------------
//
// §28.9 (RETURN × ARGUMENT-PASS, nested): the inner concat `"ab"+"cd"` is
// released after `Build` returns (a post-call release on Build's argument);
// `Build`'s own return mints +1 for the result; that result is passed to `Len`
// and released after `Len` returns. Two independent post-call releases, each
// balancing exactly one +1 producer.
const char* kNestedCallChainBothRelease =
    "module M; class C "
    "  public static method Build(s: string): string return s + \"!\"; end Build; "
    "  public static method Len(s: string): int return s.Length; end Len; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Len(C.Build(\"ab\" + \"cd\")); "  // "abcd!" => 5
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: statement-level call with producer arg -----------------------
//
// §28.9: the call is used as a statement (result discarded). The concat's +1 is
// released after the call. Looped → no accumulation.
const char* kStatementCallProducerArgReleased =
    "module M; class C "
    "  public static method SideEffect(s: string): int return s.Length; end SideEffect; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 100000 do "
    "      _ := C.SideEffect(\"eff\" + \"ect\"); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// ---- Programs: virtual (vtable) method call with producer arg ---------------
//
// §28.9 / §12: vtable dispatch does not change the argument-pass convention —
// the concat temporary is still released after the virtual call.
const char* kVirtualCallProducerArgReleased =
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
    "    total := total + b.Consume(\"vtab\" + \"cd\"); "  // "vtabcd" => 6, +1 via override => 7
    "    i := i + 1; "
    "  end; "
    "  if total = 700000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs: interface (itable) method call with producer arg -------------
//
// §28.9 / §13: itable dispatch: same argument-pass convention.
const char* kItableCallProducerArgReleased =
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
    "    total := total + ic.Consume(\"itab\" + \"cd\"); "  // "itabcd" => 6
    "    i := i + 1; "
    "  end; "
    "  if total = 600000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs: constructor with producer arg (double-free canary) -----------
//
// §28.9: `new Holder("ct" + "or")` passes a concat to the constructor. Inside
// the ctor, `this.Name := n` sees `n` (a param, not a +1 producer) → RETAINS
// (rc=2). The post-ctor release drops rc to 1; the field holds its +1, released
// on the owner's disposal. No double-free. Looped + using var → LSan-clean.
const char* kConstructorProducerArgFieldStoreLoop =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(\"ct\" + \"or\"); "
    "    total := total + h.Name.Length; "  // "ctor" => 4
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs (#2866): record-ctor literal positional arg (CONTROL) --------
//
// §9.7 / §28.9: a literal at a positional record-ctor argument position is a
// sentinel — no producer, no leak, no release. Pins the literal path against
// any future over-broad fix that would release a sentinel and corrupt it.
const char* kRecordCtorLiteralPositionalArg =
    "module M; public record Named(Tag: string, N: int); "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 1000 do "
    "    i := i + 1; "
    "var nm := new Named(\"literal\", i); "
    "    total := total + nm.N; "
    "  end; "
    "  if total = 500500 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs (#2866): record-ctor concat positional arg (LEAKING BEFORE) ----
//
// §9.7 / §28.3 / §28.9: a +1 string producer (`\"ct\" + \"or\"`) passed directly
// as a positional record-ctor argument. §9.7 makes record reference fields
// BORROWS — the record neither retains nor disposes — and §28.3 makes the
// temporary an unbound owned temporary bound to no local. Before the fix: 675
// bytes leaked across 19 of 20 allocations in the issue's loop-amplified repro.
// After the fix: the stored +1 is transferred to the slot via retain+release;
// the slot's scope-end string-field release closes the loop. Per-iteration
// `nm.Tag.Length` proves the field survives the construction-site release.
const char* kRecordCtorConcatPositionalArg =
    "module M; public record Named(Tag: string, N: int); "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 1000 do "
    "    i := i + 1; "
    "    var nm := new Named(\"ct\" + \"or\", i); "
    "    total := total + nm.Tag.Length + nm.N; "  // "ctor" => 4
    "  end; "
    "  if total = 500500 + 4000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs (#2866): record-ctor interpolation positional arg --------------
//
// §9.7 / §28.3 / §28.9: sister test of the concat case, exercising the
// interpolation branch of `isPlusOneStringProducer`. The interpolated string
// `\"t={i}\"` is a fresh rc=1 producer, passed directly as a positional
// record-ctor argument — same fix, different producer shape. The total is the
// sum of (length of "t=" + decimal-width(i)) over i=1..1000: 9*3 + 90*4 +
// 900*5 + 1*6 = 4893, plus the sum of i (500500).
const char* kRecordCtorInterpPositionalArg =
    "module M; public record Named(Tag: string, N: int); "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 1000 do "
    "    i := i + 1; "
    "    var nm := new Named($\"t={i}\", i); "
    "    total := total + nm.Tag.Length + nm.N; "
    "  end; "
    "  if total = 500500 + 4893 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs (#2866 CARDINAL): explicit ctor with field-store, no double-free
//
// §9.7 / §28.3 / §28.9: a +1 string producer (`\"ct\" + \"or\"`) passed as an
// EXPLICIT-ctor argument. The inlined ctor body field-stores the parameter
// into the record's `Tag` field (retains, rc→2 by #2712's
// `retainAssignedStringParams`). The post-ctor release takes rc→1 — the field's
// +1 survives, and a read-back `r.Tag.Length` proves the value is live. No UAF,
// no double-free. detect_leaks=0 tolerates any unrelated residual leak.
const char* kRecordCtorExplicitCtorFieldStoreNoDoubleFree =
    "module M; public record Named "
    "  public Tag: string; "
    "  public N: int; "
    "  public constructor(t: string, n: int) "
    "    this.Tag := t; this.N := n; "
    "  end constructor; "
    "end Named; "
    "class C public static method Main(): int "
    "  var r := new Named(\"ct\" + \"or\", 7); "
    "  if r.Tag.Length = 4 and r.N = 7 then return 0; end; "
    "  return 1; "
    "end Main; end C;";

// ---- Programs: method-return arg to field-store callee (double-free canary) -
//
// §28.9: `Build()` returns +1; `h.Set(C.Build())` passes it to Set, which
// stores it into a field (retains, rc=2). Post-call release → rc=1. No UAF/DF.
const char* kMethodReturnArgFieldStoreNoDoubleFree =
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
    "      h.Set(C.Build()); "
    "      total := total + h.Name.Length; "  // "built" => 5
    "      i := i + 1; "
    "    end; "
    "    if total = 500000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: range-slice direct arg --------------------------------------
//
// §3.4 / §28.9: `src[0..3]` is a +1 producer (fresh allocation from
// bz_string_slice). Passed directly as an argument → released after the call.
const char* kSliceDirectArgReleased =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var src: string := \"hello\"; "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      total := total + C.Consume(src[0..3]); "  // slice "hell" => 4
    "      i := i + 1; "
    "    end; "
    "    if total = 400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs: local string arg (NOT a +1 producer at call site) -----------
//
// §28.9 / #1430: a LOCAL string `var s := "ab"+"cd"` owns +1. When passed as
// an argument, `s` is an IDENTIFIER (not a +1 producer at the call site) — no
// post-call release fires; the local's own scope-end release handles it. Until
// #1430 the escape walk marked s as escaped when passed to ANY call and its +1
// was conservatively leaked; the walk now treats a bare identifier in a plain
// borrow position of a compiled user method as a call-bounded read, so the
// scope-end release fires and the shape is LSan-CLEAN. Correctness (value
// round-trips) and no-double-free are asserted by the same run.
const char* kLocalStringArgNotAProducerControl =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var s: string := \"ab\" + \"cd\"; "  // producer local — NOT a producer at call site
    "    return C.Consume(s); "               // passes the identifier, not the expression
    "  end Main; "
    "end C;";

// ---- Programs (#1430): local passed to borrow-only callee, looped ----------
//
// The s4/xmod shape mono-module, looped: the +1 producer local is passed to a
// borrow-only user method each iteration; post-#1430 each iteration's local is
// released at its scope end → bounded memory, LSan-clean.
const char* kLocalArgBorrowOnlyLooped =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      do var s := \"ab\" + \"cd\"; total := total + C.Consume(s); end; "
    "      i := i + 1; "
    "    end; "
    "    if total = 400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ---- Programs (#1430 CARDINAL): local arg to FIELD-STORE callee -------------
//
// The double-free guard for the walker relaxation: the callee STORES the
// borrowed local into a field (field-store retains, rc→2); the caller's
// scope-end release (new under #1430) drops rc→1 — the field's +1 survives and
// the value is READ BACK AFTER the caller's release point would have run for
// an inner scope. No UAF, no double-free; the holder's disposal releases the
// field's +1.
const char* kLocalArgFieldStoreNoDoubleFree1430 =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\"; end constructor; "
    "  public method Set(s: string) this.Name := s; end Set; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  do var s := \"ab\" + \"cd\"; h.Set(s); end; "  // s released at inner scope end
    "  if h.Name.Length = 4 then return 0; end; "     // field's +1 still live
    "  return 1; "
    "end Main; end C;";

// ---- Programs (#1430 guard): local used in a call INSIDE a lambda ----------
//
// The relaxation must NOT apply through a lambda: the env captures s and
// outlives the scope, so s stays ESCAPED (retained conservatively, tolerated
// leak) — asserting correctness and no UAF/DF when the closure runs after the
// producing statement.
const char* kLocalArgInsideLambdaStaysEscaped =
    "module M; class C "
    "  public static method Consume(s: string): int return s.Length; end Consume; "
    "  public static method Main(): int "
    "    var s := \"ab\" + \"cd\"; "
    "    var f := lambda() => C.Consume(s); "   // env captures s
    "    var n := f(); "
    "    if n = 4 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// ============= Tests: argument-pass retain (#429 §28.9 §4.6) =================

// §28.9 / §4.6 / §28.3: concat passed directly as argument to a borrow-only
// callee. Released after the call → reclaimed each iteration → LSan-clean.
BEZEL_TEST(codegen_argpass_string_429, concat_direct_arg_borrow_only_28_9) {
    expectValueAndLeakFree(kConcatDirectArgBorrowOnlyCallee, 0);
}

// §28.9 / §28.3 (RETURN × ARG-PASS): user-method return string passed directly
// as argument to a borrow-only callee. Both returns (outer call) are balanced:
// Build mints +1 → Consume receives borrow → caller releases after Consume.
BEZEL_TEST(codegen_argpass_string_429, method_return_direct_arg_borrow_only_28_9) {
    expectValueAndLeakFree(kMethodReturnDirectArgBorrowOnlyCallee, 0);
}

// §28.9 / §4.6: interpolation in argument position. Released after the call.
BEZEL_TEST(codegen_argpass_string_429, interp_direct_arg_borrow_only_28_9) {
    expectValueAndLeakFree(kInterpDirectArgBorrowOnlyCallee, 0);
}

// §28.9: multiple producer args in one call — each released after the call.
BEZEL_TEST(codegen_argpass_string_429, multiple_producer_args_one_call_28_9) {
    expectValueAndLeakFree(kMultipleProducerArgsCall, 0);
}

// §28.9 (CARDINAL double-free canary): concat arg to field-store callee.
// Field-store retains the param (rc→2); post-call release (rc→1). No double-free.
BEZEL_TEST(codegen_argpass_string_429, concat_arg_field_store_no_double_free_28_9) {
    // Use expectValueAndNoCorruption: the field-store retain makes this clean
    // on the memory-safety side (no UAF/DF). The loop stresses the pattern;
    // the object's disposal releases the final field value.
    expectValueAndNoCorruption(kConcatArgFieldStoreNoDoubleFree, 0);
}

// §28.9 (CONTROL — sentinel no-op): literal arg. Release is a no-op on the
// sentinel. This was already clean before the fix; verifies no regression.
BEZEL_TEST(codegen_argpass_string_429, literal_arg_sentinel_control_28_9) {
    expectValueAndLeakFree(kLiteralArgSentinelControl, 0);
}

// §28.9 (RETURN × ARG-PASS, nested): foo(bar(s + "x")) — two post-call
// releases, one for the concat arg to bar and one for bar's return to foo.
BEZEL_TEST(codegen_argpass_string_429, nested_call_chain_both_release_28_9) {
    expectValueAndLeakFree(kNestedCallChainBothRelease, 0);
}

// §28.9: statement-level call with producer arg. Producer released after call.
BEZEL_TEST(codegen_argpass_string_429, statement_call_producer_arg_released_28_9) {
    expectValueAndLeakFree(kStatementCallProducerArgReleased, 0);
}

// §28.9 / §12: virtual (vtable) dispatch — same argument-pass convention.
BEZEL_TEST(codegen_argpass_string_429, virtual_call_producer_arg_released_28_9) {
    expectValueAndLeakFree(kVirtualCallProducerArgReleased, 0);
}

// §28.9 / §13: interface (itable) dispatch — same argument-pass convention.
BEZEL_TEST(codegen_argpass_string_429, itable_call_producer_arg_released_28_9) {
    expectValueAndLeakFree(kItableCallProducerArgReleased, 0);
}

// §28.9: constructor with +1 producer arg, constructor stores param into field.
// Field-store retains (rc→2); post-ctor release (rc→1); disposal releases field.
BEZEL_TEST(codegen_argpass_string_429, ctor_producer_arg_field_store_loop_28_9) {
    expectValueAndLeakFree(kConstructorProducerArgFieldStoreLoop, 0);
}

// §28.9 (CARDINAL double-free canary): user-method-return arg to field-store
// callee. Build's +1 is borrowed by Set; Set's field-store retains (rc→2);
// post-call release (rc→1). Final value released on disposal. No double-free.
BEZEL_TEST(codegen_argpass_string_429, method_return_arg_field_store_no_df_28_9) {
    expectValueAndNoCorruption(kMethodReturnArgFieldStoreNoDoubleFree, 0);
}

// §3.4 / §28.9: range-slice in argument position. Slice is a +1 producer;
// released after the call → no per-iteration heap growth.
BEZEL_TEST(codegen_argpass_string_429, slice_direct_arg_released_28_9) {
    expectValueAndLeakFree(kSliceDirectArgReleased, 0);
}

// §28.9 (CONTROL — local identifier, no post-call release): passing a local
// string (not an inline producer) is NOT a +1 producer at the call site, so
// no post-call release is emitted. Correctness check + no double-free.
BEZEL_TEST(codegen_argpass_string_429, local_string_arg_not_a_producer_control_28_9) {
    // #1430: the walk now treats the bare-identifier borrow-pass as a
    // call-bounded read, so s keeps its scope-end release → LSan-clean
    // (upgraded from the pre-#1430 tolerated-leak assertion).
    expectValueAndLeakFree(kLocalStringArgNotAProducerControl, 4);
}

// #1430: the s4/xmod shape mono-module, looped — the producer local passed to
// a borrow-only user method is released at its scope end each iteration.
BEZEL_TEST(codegen_argpass_string_429, local_arg_borrow_only_looped_leak_free_1430) {
    expectValueAndLeakFree(kLocalArgBorrowOnlyLooped, 0);
}

// #1430 (CARDINAL double-free guard for the walker relaxation): the callee
// field-stores the borrowed local (retain, rc→2); the caller's scope-end
// release drops rc→1; the field read-back proves the +1 survives. detect_leaks
// off — the holder's field release at disposal is object-bookkeeping the
// harness tolerates, the assertion here is NO UAF/DF + correct value.
BEZEL_TEST(codegen_argpass_string_429, local_arg_field_store_no_double_free_1430) {
    expectValueAndNoCorruption(kLocalArgFieldStoreNoDoubleFree1430, 0);
}

// #1430 (negative guard): a call-argument use INSIDE a lambda body must stay
// ESCAPED (the env outlives the scope); correctness + no UAF when the closure
// runs later. The conservative retained +1 may leak — tolerated by design.
BEZEL_TEST(codegen_argpass_string_429, local_arg_inside_lambda_stays_escaped_1430) {
    expectValueAndNoCorruption(kLocalArgInsideLambdaStaysEscaped, 0);
}

// ============= Tests: record-construction #2866 §9.7 / §28.3 / §28.9 =========

// §28.9 / §9.7 (CONTROL — literal positional record-ctor arg): a literal is a
// sentinel — no producer, no leak before the fix and no release under it. Pins
// the literal path against any future over-broad fix that would release it.
BEZEL_TEST(codegen_argpass_string_2866, literal_positional_record_ctor_arg_9_7) {
    expectValueAndLeakFree(kRecordCtorLiteralPositionalArg, 0);
}

// §9.7 / §28.3 / §28.9: a +1 string producer passed DIRECTLY as a positional
// record-ctor argument. §9.7 makes the record's reference fields BORROWS — the
// record neither retains nor disposes — and §28.3 makes the temporary an unbound
// owned temporary bound to no local. Before the fix: 675 bytes / 38 alloc leaked
// across 19 of 20 iterations in the issue's loop-amplified repro. After: clean.
BEZEL_TEST(codegen_argpass_string_2866, concat_positional_record_ctor_arg_9_7) {
    expectValueAndLeakFree(kRecordCtorConcatPositionalArg, 0);
}

// §9.7 / §28.3 / §28.9: interpolated-string producer passed directly as a
// positional record-ctor argument. Same fix shape as the concat case — sister
// test pinning the interpolation branch of `isPlusOneStringProducer`.
BEZEL_TEST(codegen_argpass_string_2866, interp_positional_record_ctor_arg_9_7) {
    expectValueAndLeakFree(kRecordCtorInterpPositionalArg, 0);
}

// §9.7 / §28.3 / §28.9 (CARDINAL double-free canary): a +1 string producer
// passed as an EXPLICIT-ctor argument that the inlined body field-stores into
// the record's own string field. The body-side retain (#2712's
// `retainAssignedStringParams`) bumps rc→2; the post-ctor release takes rc→1;
// the field's +1 survives. No UAF / double-free. detect_leaks=0 tolerates any
// residual bookkeeping leak from unrelated disposal paths.
BEZEL_TEST(codegen_argpass_string_2866, explicit_ctor_field_store_no_double_free_9_7) {
    expectValueAndNoCorruption(kRecordCtorExplicitCtorFieldStoreNoDoubleFree, 0);
}
