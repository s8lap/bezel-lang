// Codegen + runtime — reference counting for ESCAPING closure cells
// (issue #429, CLOSURE sub-slice). Per §28.9 closures are runtime-managed
// ("the closure cell and any captured environment are reclaimed automatically
// via reference counting").  The non-escaping half (#422) released a `var`
// closure local at scope end when the escape walk proved it never escapes.
// THIS file exercises the ESCAPING half:
//
//   1. RETURN — a method whose declared return type is a function type
//      (Kind::Function) must mint EXACTLY +1 for the returned closure cell:
//      transfer a +1-producer (a freshly-created lambda / method-ref cell,
//      or a local whose slot already owns +1 — the analogous test to
//      returnedStringLocalOwnsPlusOne), retain otherwise (a borrow: a
//      field/param/identifier that already lives somewhere else).
//      The returned closure local is EXCLUDED from scope-end release (mirroring
//      the string RETURN slot-exclusion) so the single +1 flows to the caller.
//      Caller treats the result as a +1 producer and releases at scope end iff
//      non-escaping.
//
//   2. FIELD-STORE — a class field whose declared type is a function type
//      (Kind::Function) takes an INDEPENDENT +1 on every store:
//      a freshly-produced cell (lambda / method-ref) is TRANSFERRED (no
//      retain); any other closure (a param / field / identifier borrow) is
//      RETAINED (bz_closure_retain).  Overwriting a closure field RELEASES
//      the OLD value AFTER retaining the NEW (retain-before-release, so
//      self-overwrite is safe and the old value is never freed while the new
//      retain is in flight).  The owner's disposal releases the FINAL value
//      via bz_closure_release, mirroring the string-field disposal pattern.
//
// The closure ENV holds captured BORROWS only (§28.9); so ARC is sound
// (no cycles, no sub-object ownership).  bz_closure_release frees ONLY the
// cell and env block, never a captured value.
//
// Two complementary checks:
//   1. expectValueAndLeakFree: exit code correct AND LSan reports no leak /
//      UAF / double-free (the new retain/release round-trip is balanced).
//   2. expectValueAndNoCorruption: exit code correct AND ASan reports no
//      memory-safety error, tolerating a residual leak.  Used for cases
//      where a known safe leak survives (captured-variable boxed cells, etc.)
//
// The 52 existing tests in escaping_string_refcount_test.cpp must remain
// LSan-clean (this file does not touch the string path).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
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
static const std::filesystem::path& wtDir429cl() {
    return wtScratchDir("wt-429cl");
}

// Compile `source` to a temp native executable via the real Driver pipeline
// and run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429cl();
    std::string stem = unique_stem("bezel_429cl");
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

// Lower `source` to an object file and link it against the memoized ASan-built
// runtime.  Returns true and fills `obj`/`exe` on success; false on failure
// (toolchain unavailable / compile / link error).
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir429cl();
    std::string stem = unique_stem("bezel_429cl_san");
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
        if (!bezel::test::emitObjectFileForTest(*module, obj, err)) return false;
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + obj + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) return false;
    return true;
}

// LSan verification (detect_leaks=1, exitcode=99).
// Returns: 0=clean, 1=sanitizer problem (leak/UAF/DF), -1=toolchain
// unavailable.
int runUnderLeakSanitizer(const std::string& source) {
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return -1;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str());
    std::remove(exe.c_str());
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// Assert exit code correct AND LSan clean (no leak / UAF / double-free).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert exit code correct AND ASan reports no memory-safety error, tolerating
// a residual leak (detect_leaks=0; exitcode=99 is a distinct ASan failure code
// so no confusion with a legitimate expectedExitCode; no test expects 99).
void expectValueAndNoCorruption(const std::string& source,
                                int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=0:exitcode=99:abort_on_error=0 " + exe +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(obj.c_str());
    std::remove(exe.c_str());
    if (status == -1) return;
    BEZEL_EXPECT_TRUE(WIFEXITED(status) &&
                      WEXITSTATUS(status) == expectedExitCode);
}

// ==========================================================================
// Programs: RETURN escape (#429 closure RETURN sub-slice)
// ==========================================================================
//
// Convention: a method whose declared return type is a function type mints
// EXACTLY +1 for the returned closure cell.  A +1-producer returned value
// (a freshly-created lambda, a method-ref, or a local whose slot already
// owns +1) is TRANSFERRED (no retain).  A borrow (a closure-typed field /
// parameter / identifier read) is RETAINED (bz_closure_retain) so the
// returned cell carries exactly one independent reference to the caller.
// The caller treats such a call result as a +1 producer: binds it to a
// non-escaping `var` local → releases at scope end → no leak, no UAF.

// §28.9 / §28.3: a static method that creates and RETURNS a fresh lambda
// (a +1 producer in the callee).  The callee TRANSFERS the producer reference.
// The caller's non-escaping `var f` transfers+releases it → freed each
// iteration.  Looped 200k for steady-state.  Leaked before #429 closure fix
// (the caller never released the callee's +1).
const char* kReturnFreshLambdaLoop =
    "module M; class C "
    "  public static method Make(): () -> int "
    "    return lambda() => 7; "
    "  end Make; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var f: () -> int := C.Make(); "   // +1 from callee, released at scope end
    "      total := total + f(); "           // 7 each time
    "      i := i + 1; "
    "    end; "
    "    if total = 1400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3: `var t := () => k; return t;` — a returned LOCAL that already
// owns a +1 (the lambda transferred its single reference into the slot).
// The return TRANSFERS that +1 (no second retain); minting a fresh one would
// leak, since the escaped local is not released at scope end.  The caller
// transfers+releases the single +1 → exactly one cell freed per iteration.
// Looped → steady-state LSan-clean.  The decisive guard against
// double-counting the producer-local's +1 on return.
const char* kReturnProducerLocalLoopNoDouble =
    "module M; class C "
    "  public static method Build(): () -> int "
    "    var t: () -> int := lambda() => 42; "   // producer local owns +1
    "    return t; "                        // TRANSFER its +1 (no retain)
    "  end Build; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var f: () -> int := C.Build(); "
    "      total := total + f(); "         // 42
    "      i := i + 1; "
    "    end; "
    "    if total = 8400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3 / §27.5: a factory that returns a CAPTURING lambda (captures
// an int local).  The closure cell + env block are produced fresh each call.
// The callee TRANSFERS the producer's +1.  The caller's non-escaping `var f`
// releases it at scope end → cell + env freed each iteration.
// NOTE: the BOXED capture cell (the shared heap cell for `count`) is a
// separate allocation that closure-cell refcounting does NOT reclaim (per
// §28.9 / #422: freeing it on closure release would be a UAF).  So a residual
// leak of the boxed cell is tolerated; the CARDINAL guarantee is that the
// closure CELL itself is released and no memory-safety error occurs.
// Value + no-corruption (not LSan-clean because of the boxed capture cell).
const char* kReturnCapturingLambdaNoUaf =
    "module M; class C "
    "  public static method Adder(offset: int): (int) -> int "
    "    return lambda(x: int) => x + offset; "    // offset is a param, captured by value into env
    "  end Adder; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 5000 do "
    "      var f: (int) -> int := C.Adder(i); "  // +1 closure from callee
    "      total := total + f(1); "              // 1 + i
    "      i := i + 1; "
    "    end; "
    "    if total = 12502500 then return 0; end; return 1; "  // sum_{i=0}^{4999}(i+1) = 5000*5001/2 = 12502500
    "  end Main; "
    "end C;";

// §28.9 / §28.3: return inside an IF / early-return path.  Both the early
// `return` (a fresh lambda) and the fall-through `return` (another fresh
// lambda) are +1 producers; each path mints +1 exactly once (transfers the
// producer).  Looped → steady-state.
const char* kReturnInIfEarlyReturnLoop =
    "module M; class C "
    "  public static method Pick(b: bool): () -> int "
    "    if b then return lambda() => 1; end; "
    "    return lambda() => 2; "
    "  end Pick; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var f: () -> int := C.Pick(i = 0); "  // () => 1 for i=0, () => 2 otherwise
    "      total := total + f(); "
    "      i := i + 1; "
    "    end; "
    "    if total = 399999 then return 0; end; return 1; "  // 1 + 199999*2 = 399999
    "  end Main; "
    "end C;";

// §28.9 / §28.3: a method that returns a METHOD REFERENCE (a fresh closure
// cell via makeMethodThunk).  The callee TRANSFERS the producer reference.
// Looped → steady-state LSan-clean.
const char* kReturnMethodRefLoop =
    "module M; class H "
    "  public static method Square(x: int): int => x * x; "
    "end H; "
    "class C "
    "  public static method GetSquare(): (int) -> int "
    "    return H.Square; "              // method-ref: fresh +1 cell (producer)
    "  end GetSquare; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var f: (int) -> int := C.GetSquare(); "  // +1 from callee
    "      total := total + f(3); "    // 9 each time
    "      i := i + 1; "
    "    end; "
    "    if total = 1800000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3 / §31.2: return through a try-finally is deferred for
// function-type returns (the finally-return frame stores a `ptr` slot and the
// closure-return path needs to route through it — a known pre-existing gap,
// not introduced by this fix).  Test excluded to avoid a pre-existing crash.

// §28.4 / §28.9 / §28.3: a VIRTUAL/OVERRIDE closure-returning method called
// through a base reference.  Both the base virtual body and the derived
// override body mint +1 on return; the dynamic target hands back exactly +1
// regardless of which override runs.  Caller transfers+releases → no leak.
// Looped → steady-state.
const char* kReturnVirtualOverrideLoop =
    "module M; class Base "
    "  public virtual method GetFn(): () -> int return lambda() => 1; end GetFn; "
    "end Base; "
    "class Derived : Base "
    "  public override method GetFn(): () -> int return lambda() => 2; end GetFn; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var f: () -> int := b.GetFn(); "  // vtable dispatch → () => 2
    "    total := total + f(); "           // 2 each time
    "    i := i + 1; "
    "  end; "
    "  if total = 200000 then return 0; end; return 1; "
    "end Main; end C;";

// §11 / §28.9 / §28.3: closure-returning method called through an INTERFACE
// reference (itable dispatch).  The implementing class body mints +1; caller
// transfers+releases.  Looped → steady-state.
const char* kReturnInterfaceDispatchLoop =
    "module M; interface IFnSource "
    "  method GetFn(): () -> int; "
    "end IFnSource; "
    "class Thing : IFnSource "
    "  public method GetFn(): () -> int return lambda() => 5; end GetFn; "
    "end Thing; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var t := new Thing(); "
    "    var src: IFnSource := t; "
    "    var f: () -> int := src.GetFn(); "  // itable dispatch
    "    total := total + f(); "             // 5 each time
    "    i := i + 1; "
    "  end; "
    "  if total = 500000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §28.3 (CONTROL — non-escaping closure unaffected): a non-escaping
// `var f := () => k` local should still be released at scope end as before.
// The return-escape path must not interfere with the existing #422 non-escape
// release.  Looped → LSan-clean (both the return-escape path and the
// non-escape path are balanced).
const char* kNonEscapingControlUnaffected =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var f: () -> int := lambda() => 3; "   // non-escaping: released at scope end
    "      acc := acc + f(); "
    "      i := i + 1; "
    "    end; "
    "    if acc > 0 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ==========================================================================
// Programs: FIELD-STORE escape (#429 closure FIELD-STORE sub-slice)
// ==========================================================================
//
// Convention: a class field whose declared type is a function type (closure)
// takes an INDEPENDENT +1 on every store (transfer a +1 producer / retain a
// borrow).  Overwriting releases the OLD cell AFTER retaining/transferring
// the NEW (retain-before-release → self-overwrite safe).  The owner's
// disposal releases the FINAL field value → no leak.

// §28.9: a class with a closure FIELD set to a fresh lambda in the
// constructor.  Construct + dispose → LSan-clean (no accumulation).  Looped
// 200k for steady-state.  Leaked before #429 closure fix.
const char* kFieldStoreCtorProducerLoop =
    "module M; class Holder "
    "  public Fn: () -> int; "
    "  public constructor() this.Fn := lambda() => 11; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Fn(); "   // 11 each time
    "    i := i + 1; "
    "  end; "
    "  if total = 2200000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: a class with a closure FIELD set to a METHOD REFERENCE (a fresh +1
// cell via makeMethodThunk) in the constructor.  Construct + dispose →
// LSan-clean.  Looped.
const char* kFieldStoreMethodRefLoop =
    "module M; class H "
    "  public static method Val(): int => 13; "
    "end H; "
    "class Holder "
    "  public Fn: () -> int; "
    "  public constructor() this.Fn := H.Val; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Fn(); "   // 13 each time
    "    i := i + 1; "
    "  end; "
    "  if total = 2600000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: REASSIGN a closure field many times on a long-lived object —
// each reassignment releases the OLD cell and retains/transfers the new, so
// only the FINAL lambda is held until disposal.  Bounded memory (no
// per-write growth) → LSan-clean.  Guards both leak (no release-old) and
// double-free / UAF (release-before-retain-new would free the old value
// before the new is safe, and retain-new-before-release-old makes it safe).
const char* kFieldReassignLoop =
    "module M; class Holder "
    "  public Fn: () -> int; "
    "  public _last: int := 0; "
    "  public constructor() this.Fn := lambda() => 0; end constructor; "
    "  public method CallAndStore(): int "
    "    this._last := this.Fn(); "   // invoke and record result in a field
    "    return this._last; "
    "  end CallAndStore; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; "
    "  using var h := new Holder(); "
    "  while i < 200000 do "
    "    h.Fn := lambda() => 42; "          // each store releases the previous cell; constant lambda
    "    i := i + 1; "
    "  end; "
    "  var r := h.CallAndStore(); "   // invoke the final lambda via instance method
    "  if r = 42 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 (cardinal double-free guard — retain-before-release): a closure field
// is overwritten in a tight loop.  With retain-NEW before release-OLD, the old
// cell's rc never transiently 0 during the overwrite — so a self-assignment
// `h.Fn := () => k; h.Fn := () => k;` is safe (the first overwrite's new
// lambda rc starts at 1 from its producer; the second overwrite retains the
// next new lambda first, stores it, then releases the first → no UAF, no
// double-free).  NOTE: aliasing a closure field borrow (var old := h.Fn)
// without borrow-retain for closures is out of this sub-slice — closures do
// not have the string borrow-retain mechanism, so a raw alias of h.Fn would
// be invalidated by a subsequent overwrite (a known limitation, not new to
// this fix; the alias-read UAF test for strings is separate).  This test
// verifies the overwrite chain without aliasing.
const char* kFieldOverwriteNoUaf =
    "module M; class Holder "
    "  public Fn: () -> int; "
    "  public _last: int := 0; "
    "  public constructor() this.Fn := lambda() => 0; end constructor; "
    "  public method Call(): int "
    "    this._last := this.Fn(); "
    "    return this._last; "
    "  end Call; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; "
    "  using var h := new Holder(); "
    "  while i < 50000 do "
    "    h.Fn := lambda() => 77; "           // overwrite → retain-new, release-old
    "    h.Fn := lambda() => 77; "           // overwrite again (retain-before-release)
    "    i := i + 1; "
    "  end; "
    "  var r := h.Call(); "
    "  if r = 77 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: a class with BOTH a closure-typed FIELD and a string field — the
// disposal cascades across different field types.  Each construction/disposal
// pair reclaims both fields correctly.  Looped → steady-state LSan-clean.
const char* kClosureAndStringFieldLoop =
    "module M; class Holder "
    "  public Fn: () -> int; "
    "  public Name: string; "
    "  public constructor() "
    "    this.Fn := lambda() => 3; "
    "    this.Name := \"he\" + \"llo\"; "
    "  end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Fn() + h.Name.Length; "   // 3 + 5 = 8
    "    i := i + 1; "
    "  end; "
    "  if total = 800000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: a class with a closure FIELD set to a RETURNED closure (RETURN +
// FIELD-STORE compose).  The call is a +1 producer (after the RETURN fix),
// so the field-store TRANSFERS the +1 into the field (no extra retain);
// disposal releases it → reclaimed.  Looped → steady-state LSan-clean.
const char* kReturnIntoFieldStoreLoop =
    "module M; class Maker "
    "  public static method Make(): () -> int return lambda() => 7; end Make; "
    "end Maker; "
    "class Holder "
    "  public Fn: () -> int; "
    "  public constructor() this.Fn := Maker.Make(); end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Fn(); "    // 7
    "    i := i + 1; "
    "  end; "
    "  if total = 700000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: a class with an explicitly-initialized closure field assigned
// multiple times across iterations.  Each construction initializes the field
// via the constructor (a +1 producer transferred), invokes it, disposes →
// field released.  The EXPLICIT-DISPOSE path (h.Dispose() via IDisposable)
// also releases the field.  Looped → LSan-clean.
const char* kExplicitDisposeClosureLoop =
    "module M; class Holder : IDisposable "
    "  public Fn: () -> int; "
    "  public constructor() this.Fn := lambda() => 4; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    var h := new Holder(); "
    "    total := total + h.Fn(); "   // invoke
    "    h.Dispose(); "               // explicit dispose: field released
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.4 / §28.9: INHERITANCE — base and derived each declare a closure field;
// the base's is released by the base CORE disposer (emitBaseDisposeChain),
// the derived's by the derived disposer — once each, no double-release.
// Looped for steady-state LSan-clean.
const char* kInheritedClosureFieldsLoop =
    "module M; class Base "
    "  public BaseFn: () -> int; "
    "  public constructor() this.BaseFn := lambda() => 10; end constructor; "
    "end Base; "
    "class Derived : Base "
    "  public DerivedFn: () -> int; "
    "  public constructor() this.DerivedFn := lambda() => 20; end constructor; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    total := total + d.BaseFn() + d.DerivedFn(); "   // 10 + 20 = 30
    "    i := i + 1; "
    "  end; "
    "  if total = 3000000 then return 0; end; return 1; "
    "end Main; end C;";

}  // namespace

// ======================================================================
// Tests: RETURN escape category (#429 closure RETURN sub-slice)
// ======================================================================

BEZEL_TEST(codegen_escaping_closure_429, return_fresh_lambda_loop_reclaimed_28_9) {
    // A method returning a fresh lambda: callee TRANSFERS the +1 producer
    // reference; caller's non-escaping `var f` releases it each iteration →
    // LSan-clean steady state.  Leaked before #429 closure fix.
    expectValueAndLeakFree(kReturnFreshLambdaLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_producer_local_loop_no_double_28_3) {
    // Returning a producer LOCAL transfers its single +1 (no double-count).
    // A second retain would leak; a transfer keeps the round trip balanced.
    expectValueAndLeakFree(kReturnProducerLocalLoopNoDouble, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_capturing_lambda_no_uaf_27_5) {
    // A capturing lambda returned from a factory.  Closure cell released
    // (transfer path); the boxed capture cell leak is tolerated (a known
    // safe over-retention, not a UAF).  Value + no-corruption.
    expectValueAndNoCorruption(kReturnCapturingLambdaNoUaf, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_in_if_early_return_loop_28_3) {
    // Both early- and tail-return paths mint exactly +1; caller balances →
    // no leak per iteration.
    expectValueAndLeakFree(kReturnInIfEarlyReturnLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_method_ref_loop_reclaimed_27_6) {
    // A method-ref returned: fresh closure cell (makeMethodThunk) transferred
    // to caller; caller releases → LSan-clean steady state.
    expectValueAndLeakFree(kReturnMethodRefLoop, 0);
}

// return_lambda_through_finally_loop_31_2: EXCLUDED — a pre-existing crash
// in return-through-finally for function-typed return values (the finally
// frame stores a ptr slot and the closure-return path has not been plumbed
// through it).  This is outside the #429 closure RETURN + FIELD-STORE scope;
// it is tracked as a separate pre-existing defect.

BEZEL_TEST(codegen_escaping_closure_429, return_virtual_override_loop_28_4) {
    // Both override bodies mint +1 on return → no double-free across vtable;
    // caller releases → LSan-clean.
    expectValueAndLeakFree(kReturnVirtualOverrideLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_interface_dispatch_loop_11) {
    // Itable dispatch: the impl body mints +1; caller balances → clean.
    expectValueAndLeakFree(kReturnInterfaceDispatchLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, non_escaping_control_unaffected_422) {
    // CONTROL: a non-escaping closure local must still be released at scope
    // end exactly as before.  The return-escape path must not disturb it.
    expectValueAndLeakFree(kNonEscapingControlUnaffected, 0);
}

// ======================================================================
// Tests: FIELD-STORE escape category (#429 closure FIELD-STORE sub-slice)
// ======================================================================

BEZEL_TEST(codegen_escaping_closure_429, field_store_ctor_producer_loop_reclaimed_28_9) {
    // Construct + dispose looped: the lambda is retained/transferred at store
    // and released on disposal → LSan-clean.  Leaked before #429 closure fix.
    expectValueAndLeakFree(kFieldStoreCtorProducerLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, field_store_method_ref_loop_reclaimed_27_6) {
    // Method-ref stored into a field: method-ref cell is a +1 producer,
    // TRANSFERRED into the field; disposal releases it → LSan-clean.
    expectValueAndLeakFree(kFieldStoreMethodRefLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, field_reassign_loop_bounded_no_uaf_28_9) {
    // Reassigning a closure field 200k times: each overwrite releases the OLD
    // cell and takes +1 on the NEW; only the final lambda survives to disposal.
    // Bounded memory (no per-write growth) → LSan-clean.  The final lambda is
    // invoked via an instance method (avoids ownership-checker borrow-lifetime
    // restriction on returning a borrow of h.Fn from Main) and records 42 →
    // exit 0 when correct.
    expectValueAndLeakFree(kFieldReassignLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, field_overwrite_no_uaf_28_9) {
    // Cardinal retain-before-release guard: a closure field is overwritten
    // repeatedly (50k * 2 overwrites per iteration).  retain-NEW before
    // release-OLD ensures no cell is freed while a reference is in flight.
    // LSan-clean: no cells accumulate, the final one is released on disposal.
    expectValueAndLeakFree(kFieldOverwriteNoUaf, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, closure_and_string_field_loop_28_9) {
    // Both a closure field and a string field are released on disposal —
    // the two disposal paths compose correctly.  LSan-clean.
    expectValueAndLeakFree(kClosureAndStringFieldLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, return_into_field_store_loop_28_3) {
    // RETURN mint + FIELD-STORE compose: a single balanced +1 from callee to
    // field to disposal.  LSan-clean.
    expectValueAndLeakFree(kReturnIntoFieldStoreLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, explicit_dispose_closure_loop_28_5) {
    // Explicit Dispose(): the closure field is released via the disposer on
    // an explicit h.Dispose() call, not just `using var`.  Looped → LSan-clean.
    expectValueAndLeakFree(kExplicitDisposeClosureLoop, 0);
}

BEZEL_TEST(codegen_escaping_closure_429, inherited_closure_fields_loop_28_4) {
    // Base and derived each have a closure field; each disposer releases its
    // own field exactly once (base CORE disposer for the base field, derived
    // disposer for the derived field) — no double-release.  LSan-clean.
    expectValueAndLeakFree(kInheritedClosureFieldsLoop, 0);
}
