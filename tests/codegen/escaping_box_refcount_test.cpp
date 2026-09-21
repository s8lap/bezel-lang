// Codegen + runtime — reference counting for ESCAPING boxed value-type wrappers
// (issue #429, BOX sub-slice). Per §8.6, a value type used where Object (or an
// interface it implements) is expected is BOXED: a heap wrapper holds the value.
// Per §28.9 boxed value types are runtime-managed — exempt from manual disposal.
// The non-escaping half (#422) released a `var` Object/interface local at scope
// end when the escape walk proved it never escapes. THIS file exercises the
// ESCAPING half — boxes that ESCAPE via RETURN or CLASS FIELD-STORE:
//
//   1. RETURN — a method whose declared return type is Object or an interface
//      and whose return expression is a value type (int/bool/record/enum/
//      nullable value), goes through boxForTarget at the return site — creating
//      a fresh rc=1 box (a +1 producer). The caller must treat the result as a
//      +1 producer: a non-escaping `var o: Object := C.GetBoxed()` local
//      releases the box at scope end → no leak. A method whose return body
//      always produces boxes via boxForTarget is a "box-return method";
//      collectBoxReleaseEligible marks a local initialized by such a call
//      as eligible for scope-end bz_box_release.
//
//   2. FIELD-STORE — a class field whose declared type is Object or an
//      interface takes an INDEPENDENT +1 on every store of a boxable value:
//      a freshly-boxed value (+1 producer from boxForTarget) is TRANSFERRED
//      (no extra retain); any other box (a borrow — a local / param / field)
//      is RETAINED (bz_box_retain). Overwriting an Object/interface field that
//      holds a box RELEASES the OLD box AFTER retaining/transferring the NEW
//      (retain-before-release → self-overwrite safe). The owner's disposal
//      releases the FINAL stored box via bz_box_release.
//
// Safety invariants:
//   - A box is ONLY ever retained/released for Object/interface CLASS fields
//     where EVERY store (across all methods of the class) supplies a boxable
//     value source type (collectBoxStorageFields pre-pass). A field that also
//     receives a class reference is excluded: class references are not boxes
//     (no bz_box layout at offset 8), and calling bz_box_release on a class
//     instance would corrupt the disposed-flag word → double-free / UAF.
//   - bz_box_retain/release are null-safe (null T? boxes to null Object;
//     a calloc-zeroed null field is a no-op → no crash on first dispose).
//   - The boxed value is a VALUE-TYPE LEAF: the box holds only a vtable*,
//     i64 rc, and value bytes. Releasing frees ONLY the wrapper — no cascade.
//
// CONTROL tests:
//   - A non-escaping boxed local (value used locally) must still be released
//     at scope end exactly as before (#422 non-escape path is untouched).
//   - A class instance coerced to Object (the #399 path) must be disposed
//     exactly once via ownership: bz_box_release must NEVER fire on it.
//
// Two complementary checks:
//   1. expectValueAndLeakFree: exit code correct AND LSan reports no leak /
//      UAF / double-free.
//   2. expectValueAndNoCorruption: exit code correct AND ASan reports no
//      memory-safety error, tolerating a residual leak. Used for the class-
//      instance-in-Object control cases.

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
static const std::filesystem::path& wtDir429bx() {
    return wtScratchDir("wt-429bx");
}

// Compile `source` to a temp native executable via the real Driver pipeline
// and run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429bx();
    std::string stem = unique_stem("bezel_429bx");
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
// runtime. Returns true and fills `obj`/`exe` on success; false on failure.
bool lowerAndLink(const std::string& source, std::string& obj,
                  std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;
    auto dir = wtDir429bx();
    std::string stem = unique_stem("bezel_429bx_san");
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
// Returns: 0=clean, 1=sanitizer problem (leak/UAF/DF), -1=toolchain unavailable.
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
    return 0;                                                       // clean
}

// Assert exit code correct AND LSan clean (no leak / UAF / double-free).
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// Assert exit code correct AND ASan reports no memory-safety error, tolerating
// a residual leak (detect_leaks=0; exitcode=99 is a distinct ASan failure code).
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
// Programs: RETURN escape (#429 box RETURN sub-slice)
// ==========================================================================
//
// Convention: a method whose declared return type is Object or an interface,
// and whose return expression is a value type, goes through boxForTarget at the
// return site — creating a fresh rc=1 box (+1 producer). The caller treats the
// result as a +1 producer: a non-escaping `var o: Object := C.GetBoxed()` local
// is released at scope end via bz_box_release → no leak.

// §28.9 / §8.6: a static method that boxes a primitive int and RETURNS it as
// Object (a +1 producer). Caller's non-escaping `var o` releases the box each
// iteration. Looped 200k for steady-state. Leaked before #429 box fix.
const char* kReturnBoxedIntLoop =
    "module M; class C "
    "  public static method GetBoxed(): Object "
    "    return 42; "
    "  end GetBoxed; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var o: Object := C.GetBoxed(); "
    "      if o is int then total := total + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if total = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §8.6: a method that boxes a RECORD value and returns it as Object.
// Callee transfers its +1 producer; caller releases → LSan-clean.
const char* kReturnBoxedRecordLoop =
    "module M; "
    "record Pt(x: int, y: int); "
    "class C "
    "  public static method GetPt(): Object "
    "    return new Pt(3, 4); "
    "  end GetPt; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      var o: Object := C.GetPt(); "
    "      if o is Pt then total := total + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §8.6: a method returning a boxed BOOL. Guards that the fix applies
// to ALL boxable primitives, not just int. Looped → steady-state LSan-clean.
const char* kReturnBoxedBoolLoop =
    "module M; class C "
    "  public static method GetFlag(b: bool): Object "
    "    return b; "
    "  end GetFlag; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var o: Object := C.GetFlag(true); "
    "      if o is bool then total := total + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if total = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §8.6: early-return (if/else) paths — each path boxes and transfers
// its +1. Caller releases → LSan-clean. Guards both code paths.
const char* kReturnBoxedEarlyReturnLoop =
    "module M; class C "
    "  public static method Pick(b: bool): Object "
    "    if b then return 1; end; "
    "    return 2; "
    "  end Pick; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var o: Object := C.Pick(i = 0); "
    "      if o is int then total := total + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if total = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §8.6 / §28.4: a VIRTUAL/OVERRIDE method returning Object (boxed int).
// Both the base virtual body and the derived override body mint +1 via
// boxForTarget; caller releases → LSan-clean.
const char* kReturnBoxedVirtualOverrideLoop =
    "module M; class Base "
    "  public virtual method GetVal(): Object return 10; end GetVal; "
    "end Base; "
    "class Derived : Base "
    "  public override method GetVal(): Object return 20; end GetVal; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var o: Object := b.GetVal(); "
    "    if o is int then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6 (CONTROL): a non-escaping `var o: Object := n` local (integer
// boxing via boxValue, not a method call) must still be released at scope end
// as before (#422 non-escape path). The box-return path must not interfere.
const char* kNonEscapingBoxedControlUnaffected =
    "module M; class C "
    "  public static method Main(): int "
    "    var i: int := 0; var acc: int := 0; "
    "    while i < 200000 do "
    "      var o: Object := i; "
    "      if o is int then acc := acc + 1; end; "
    "      i := i + 1; "
    "    end; "
    "    if acc = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// ==========================================================================
// Programs: FIELD-STORE escape (#429 box FIELD-STORE sub-slice)
// ==========================================================================
//
// Convention: a class Object/interface field where EVERY store in the class
// uses a boxable value source takes an INDEPENDENT +1 on every store.
// A freshly-boxed +1 producer is TRANSFERRED (no extra retain); anything
// else is RETAINED (bz_box_retain). Overwriting releases the OLD box AFTER
// retaining/transferring the NEW. The owner's disposal releases the FINAL box.

// §28.9 / §8.6: a class with an Object field set to a boxed int in the
// constructor. Construct + dispose → LSan-clean. Looped 200k for steady-state.
// Leaked before #429 box fix.
const char* kFieldStoreCtorBoxedIntLoop =
    "module M; class Holder "
    "  public Val: Object; "
    "  public constructor() this.Val := 42; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    if h.Val is int then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 200000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6: a class with an Object field set to a boxed RECORD.
// Construct + dispose → LSan-clean.
const char* kFieldStoreCtorBoxedRecordLoop =
    "module M; "
    "record Sz(w: int, h: int); "
    "class Holder "
    "  public Val: Object; "
    "  public constructor() this.Val := new Sz(10, 20); end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    if h.Val is Sz then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6: REASSIGN an Object field many times — each reassignment
// releases the OLD box and takes +1 on the NEW. Only the FINAL box survives
// to disposal. Bounded memory (no per-write growth) → LSan-clean.
const char* kFieldReassignBoxLoop =
    "module M; class Holder "
    "  public Val: Object; "
    "  public _count: int := 0; "
    "  public constructor() this.Val := 0; end constructor; "
    "  public method Bump(): int "
    "    this._count := this._count + 1; "
    "    return this._count; "
    "  end Bump; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; "
    "  using var h := new Holder(); "
    "  while i < 200000 do "
    "    h.Val := i; "
    "    i := i + 1; "
    "  end; "
    "  var r := h.Bump(); "
    "  if r = 1 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6 (cardinal double-free guard): an Object field is overwritten in
// a tight loop. retain-NEW before release-OLD ensures no box is freed while a
// reference is in flight. LSan-clean: no boxes accumulate, the final one is
// released on disposal.
const char* kFieldOverwriteBoxNoUaf =
    "module M; class Holder "
    "  public Val: Object; "
    "  public constructor() this.Val := 0; end constructor; "
    "  public method Check(): bool "
    "    return (this.Val is int); "
    "  end Check; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; "
    "  using var h := new Holder(); "
    "  while i < 50000 do "
    "    h.Val := 77; "
    "    h.Val := 77; "
    "    i := i + 1; "
    "  end; "
    "  var ok := h.Check(); "
    "  if ok then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6: RETURN + FIELD-STORE compose. Callee's +1 producer is
// TRANSFERRED into the field (no extra retain); disposal releases → reclaimed.
const char* kReturnIntoBoxFieldStoreLoop =
    "module M; class Maker "
    "  public static method Make(): Object return 99; end Make; "
    "end Maker; "
    "class Holder "
    "  public Val: Object; "
    "  public constructor() this.Val := Maker.Make(); end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    if h.Val is int then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §8.6: explicit Dispose() path — the Object field box is released
// via the disposer on an explicit h.Dispose() call. Looped → LSan-clean.
const char* kExplicitDisposeBoxFieldLoop =
    "module M; class Holder : IDisposable "
    "  public Val: Object; "
    "  public constructor() this.Val := 55; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    var h := new Holder(); "
    "    if h.Val is int then total := total + 1; end; "
    "    h.Dispose(); "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.4 / §28.9 / §8.6: INHERITANCE — base and derived each declare an Object
// field storing a box. The base's field is released by the base CORE disposer,
// the derived's by the derived disposer — once each, no double-release.
const char* kInheritedBoxFieldsLoop =
    "module M; class Base "
    "  public BaseVal: Object; "
    "  public constructor() this.BaseVal := 10; end constructor; "
    "end Base; "
    "class Derived : Base "
    "  public DerivedVal: Object; "
    "  public constructor() this.DerivedVal := 20; end constructor; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    if d.BaseVal is int then total := total + 1; end; "
    "    if d.DerivedVal is int then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 200000 then return 0; end; return 1; "
    "end Main; end C;";

// ==========================================================================
// Programs: CONTROL — class-instance-in-Object regression guards
// ==========================================================================
//
// CRITICAL SAFETY: a class INSTANCE coerced to Object (the #399 path) has a
// reference static type at the assignment site → NOT a box store → the field
// is excluded from boxStorageFields_ → bz_box_release is NOT emitted at
// disposal. The class instance is disposed via ownership exactly once.
// If the discriminator were unsound (box-release on a class instance), it
// would corrupt offset 8 (the disposed-flag word) → double-free / UAF.

// §28.9 / #399 / CONTROL: an Object-typed CLASS field holding a class
// reference. The store is a reference source → not in boxStorageFields_ →
// no bz_box_release in the disposer. The instance is disposed via ownership.
// ASan/LSan clean: no double-free, no corruption at offset 8.
const char* kClassRefInObjectFieldNoDoubleDispose =
    "module M; "
    "class Dog end Dog; "
    "class Container "
    "  public _ref: Object; "
    "  public constructor(d: Dog) this._ref := d; end constructor; "
    "end Container; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var d := new Dog(); "
    "    using var c := new Container(d); "
    "    if c._ref is Dog then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 50000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / #422 / CONTROL: a `var o: Object := classInstance` local (class ref,
// NOT a box). The local must NOT receive a bz_box_release at scope end. The
// class instance is disposed via ownership. ASan/LSan clean: no double-free.
const char* kClassRefInObjectLocalNoBoxRelease =
    "module M; "
    "class Cat end Cat; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var cat := new Cat(); "
    "    var o: Object := cat; "
    "    if o is Cat then total := total + 1; end; "
    "    i := i + 1; "
    "  end; "
    "  if total = 50000 then return 0; end; return 1; "
    "end Main; end C;";

}  // namespace

// ==========================================================================
// Tests: RETURN escape category (#429 box RETURN sub-slice)
// ==========================================================================

BEZEL_TEST(codegen_escaping_box_429, return_boxed_int_loop_reclaimed_28_9) {
    // A method returning a boxed int: callee TRANSFERS the fresh +1 box;
    // caller's non-escaping local releases it each iteration → LSan-clean.
    // Leaked before #429 box fix.
    expectValueAndLeakFree(kReturnBoxedIntLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, return_boxed_record_loop_reclaimed_8_6) {
    // A method returning a boxed record: the +1 box transferred to caller,
    // released → steady-state LSan-clean.
    expectValueAndLeakFree(kReturnBoxedRecordLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, return_boxed_bool_loop_reclaimed_8_6) {
    // Non-int boxable primitive (bool) returned as Object: same round-trip.
    // Guards that the fix applies to ALL boxable primitives.
    expectValueAndLeakFree(kReturnBoxedBoolLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, return_boxed_early_return_loop_28_9) {
    // Both if-branch and fall-through return paths box and transfer +1;
    // caller releases → no leak per iteration.
    expectValueAndLeakFree(kReturnBoxedEarlyReturnLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, return_boxed_virtual_override_loop_28_4) {
    // Both virtual and override bodies mint +1 via boxForTarget; vtable
    // dispatch does not break box accounting; caller releases → clean.
    expectValueAndLeakFree(kReturnBoxedVirtualOverrideLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, non_escaping_control_unaffected_422) {
    // CONTROL: the #422 non-escape release for `var o: Object := n` must still
    // fire. The box-return path must not interfere.
    expectValueAndLeakFree(kNonEscapingBoxedControlUnaffected, 0);
}

// ==========================================================================
// Tests: FIELD-STORE escape category (#429 box FIELD-STORE sub-slice)
// ==========================================================================

BEZEL_TEST(codegen_escaping_box_429, field_store_ctor_boxed_int_loop_28_9) {
    // Construct + dispose looped: the int box is transferred into the field
    // at construction and released on disposal → LSan-clean. Leaked before fix.
    expectValueAndLeakFree(kFieldStoreCtorBoxedIntLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, field_store_ctor_boxed_record_loop_8_6) {
    // Record box stored in Object field: transferred at construction, released
    // on disposal → LSan-clean.
    expectValueAndLeakFree(kFieldStoreCtorBoxedRecordLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, field_reassign_box_loop_bounded_28_9) {
    // Reassigning an Object field 200k times: each overwrite releases the OLD
    // box and takes +1 on the NEW. Only the final box survives to disposal.
    // Bounded memory → LSan-clean.
    expectValueAndLeakFree(kFieldReassignBoxLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, field_overwrite_box_no_uaf_28_9) {
    // Cardinal retain-before-release guard: Object field overwritten in a
    // tight loop. No box freed while a reference is in flight. LSan-clean.
    expectValueAndLeakFree(kFieldOverwriteBoxNoUaf, 0);
}

BEZEL_TEST(codegen_escaping_box_429, return_into_box_field_store_loop_28_3) {
    // RETURN mint + FIELD-STORE compose: a single balanced +1 flows from
    // callee → field → disposal. LSan-clean.
    expectValueAndLeakFree(kReturnIntoBoxFieldStoreLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, explicit_dispose_box_field_loop_28_5) {
    // Explicit Dispose(): the Object field box is released via the disposer
    // on an explicit h.Dispose() call. Looped → LSan-clean.
    expectValueAndLeakFree(kExplicitDisposeBoxFieldLoop, 0);
}

BEZEL_TEST(codegen_escaping_box_429, inherited_box_fields_loop_28_4) {
    // Base and derived each have an Object field with a box; each disposer
    // releases its own field exactly once (no double-release). LSan-clean.
    expectValueAndLeakFree(kInheritedBoxFieldsLoop, 0);
}

// ==========================================================================
// Tests: CONTROL — class-instance-in-Object safety (regression guards)
// ==========================================================================

BEZEL_TEST(codegen_escaping_box_429, class_ref_in_object_field_no_double_dispose) {
    // CARDINAL SAFETY: Object-typed CLASS field holding a class reference
    // (not a box) must NOT receive bz_box_release in the disposer. The class
    // instance is disposed via ownership exactly once. ASan/LSan clean.
    expectValueAndNoCorruption(kClassRefInObjectFieldNoDoubleDispose, 0);
}

BEZEL_TEST(codegen_escaping_box_429, class_ref_in_object_local_no_box_release) {
    // CARDINAL SAFETY: `var o: Object := classInstance` must NOT receive
    // bz_box_release at scope end. Disposed via ownership. ASan/LSan clean.
    expectValueAndNoCorruption(kClassRefInObjectLocalNoBoxRelease, 0);
}
