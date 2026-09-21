// Codegen + runtime — reference counting for ESCAPING runtime-managed strings
// (issue #429). Per §4.6 a `string` is runtime-managed ("its memory is
// reclaimed automatically"); per §28.9 strings are exempt from manual disposal;
// per §28.8 strings are immutable, reference-shared LEAVES, so acyclic
// reference counting is sound. The NON-escaping half (#422) released a `var`
// string local at scope end when a conservative walk proved it never escapes.
// THIS file exercises the ESCAPING half: a string that is stored into a class
// FIELD / auto-property (this increment) carries an INDEPENDENT +1 — retained at
// store (or transferred from a +1 producer), released on the owner's disposal
// and on reassignment. So a disposed object's string field is RECLAIMED (no
// leak) and overwriting / self-assigning a field never frees a value still in
// use (no use-after-free / double-free).
//
// Ownership convention (#429), STRING fields of a CLASS only:
//   - A heap string carries +1 from its producer.
//   - Store into a string class field/auto-property: the field gets an
//     INDEPENDENT +1 — a fresh +1 producer (`+` / interpolation / range-slice)
//     is TRANSFERRED (no retain); anything else (borrow / call / literal /
//     identifier) is RETAINED. Reassignment releases the OLD value after
//     retaining the NEW (retain-before-release ⇒ self-assignment safe).
//   - Object disposal releases every string field exactly once (then nulls the
//     slot ⇒ a re-entrant / double dispose is a no-op).
//   - A RECORD's string fields are BORROWS (§9: "the record does not own what
//     they point to") and a record has no disposer (§9.7) — left untouched.
//
// Two complementary checks, mirroring string_refcount_test.cpp:
//   1. expectValueAndLeakFree: native exit code is right AND LSan reports no
//      leak / UAF / double-free (the reclaim now works → clean).
//   2. expectValueAndNoCorruption: native exit code is right AND ASan reports no
//      MEMORY-SAFETY error, tolerating a residual leak (for the still-deferred
//      escape categories whose strings remain conservatively leaked).
// Both skip gracefully when no ASan toolchain is available.

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
static const std::filesystem::path& wtDir429() {
    return wtScratchDir("wt-429");
}

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir429();
    std::string stem = unique_stem("bezel_429");
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
// runtime. Returns the executable path through `exe` and the object path through
// `obj` (caller cleans up), or false on any failure (toolchain unavailable /
// compile / link error). `extraAsan` lets the caller toggle detect_leaks via the
// link itself (sanitizer is always ASan; leak detection is set at run time).
bool lowerAndLink(const std::string& source, std::string& obj, std::string& exe) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return false;  // toolchain unavailable
    auto dir = wtDir429();
    std::string stem = unique_stem("bezel_429_san");
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

// LeakSanitizer verification (detect_leaks=1). Returns 0 clean, 1 sanitizer
// problem (leak/UAF/double-free), -1 toolchain unavailable (SKIP).
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

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak / UAF / double-free.
void expectValueAndLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean (no leak, no UAF/DF)
}

// Assert correctness AND that ASan reports no MEMORY-SAFETY error (UAF /
// double-free), TOLERATING a residual leak. detect_leaks=0; exitcode=99 gives
// ASan a distinct failure code so a UAF can never be confused with the program
// legitimately returning expectedExitCode.
void expectValueAndNoCorruption(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    std::string obj, exe;
    if (!lowerAndLink(source, obj, exe)) return;  // toolchain unavailable: skip
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

// --- Programs: STRING-into-FIELD escape (#429 field-store category) -------

// §28.9 / §4.6: a +1 producer (`+`) stored into a class field through the
// constructor, then the owning object disposed (using var). The field
// TRANSFERS the producer's reference; disposal releases it → reclaimed, clean.
// Looped 200k times: the object (and its field string) is freed each iteration,
// so steady-state LSan-clean. Leaked before #429.
const char* kFieldStoreCtorProducerLoop =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"fie\" + \"ld\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Name.Length; "
    "    i := i + 1; "
    "  end; "
    "  if total = 1000000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §4.6: a string field initialized by a DEFAULT FIELD INITIALIZER (a +1
// producer), object disposed. The initializer transfers the producer reference;
// disposal releases it. Looped for steady state → LSan-clean.
const char* kFieldDefaultInitProducerLoop =
    "module M; class Holder "
    "  public Name: string := \"de\" + \"fault\"; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Name.Length; "
    "    i := i + 1; "
    "  end; "
    "  if total = 1400000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §4.6 / §9.2: an AUTO-PROPERTY of string type (`property Name: string;`,
// no accessor body — backed by a synthesized field), set in the constructor with
// a +1 producer, object disposed each iteration → reclaimed, LSan-clean.
const char* kAutoPropertyStoreLoop =
    "module M; class Holder "
    "  public property Name: string; "
    "  public constructor() this.Name := \"ab\" + \"cde\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Name.Length; "
    "    i := i + 1; "
    "  end; "
    "  if total = 1000000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: REASSIGN a string field many times on a long-lived object — every
// reassignment releases the OLD value and retains/transfers the new, so only the
// final value is held until disposal. Bounded memory (no per-write growth) →
// LSan-clean. Releasing the old value too early (before the new store) or not at
// all would be a UAF or a leak respectively; this guards both.
const char* kFieldReassignLoop =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  var i: int := 0; "
    "  while i < 200000 do "
    "    h.Name := \"x\" + \"y\"; "          // each store frees the previous
    "    i := i + 1; "
    "  end; "
    "  return h.Name.Length; "               // "xy" => 2
    "end Main; end C;";

// §28.9 (cardinal UAF guard): a string field is read into a local AFTER the
// field is overwritten — the local must still see valid memory. With retain-new-
// release-old the old value's rc only drops when nothing else holds it; here the
// local aliases the old value, so it stays alive while read. Value + no-corruption
// (the local aliases without its own +1; a residual leak is tolerated, a UAF is
// not). Returns the OLD value's length read through the alias.
const char* kFieldOverwriteAliasReadNoUaf =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"first0\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  var old: string := h.Name; "          // alias the field's value
    "  h.Name := \"second\"; "               // overwrite the field
    "  return old.Length; "                  // "first0" => 6, must be intact
    "end Main; end C;";

// §28.9 (cardinal double-free guard): LOCAL SELF-ASSIGNMENT `s := s`. With
// retain-before-release the value's rc dips +1 then back to its prior level,
// never transiently 0 — so the just-stored pointer is never a dangler. The
// reassignment marks `s` escaped (so it leaks its retained value rather than
// double-free), but the cardinal point is NO double-free / UAF. (A `h.Name :=
// h.Name` field self-assign is rejected by §28.3 ownership as a borrow stored
// into a field, so the local form is the valid analogue.) Value + no-corruption.
const char* kLocalSelfAssign =
    "module M; class C public static method Main(): int "
    "  var s: string := \"se\" + \"lf\"; "
    "  s := s; "                             // self-assignment: no double-free
    "  return s.Length; "                    // "self" => 4
    "end Main; end C;";

// §28.9: a string field set by ordinary assignment after construction stores
// with retain/transfer, released on the (single) object's disposal → the
// field string is reclaimed, LSan-clean. The field is `string?` so leaving
// it at its implicit null default until the assignment is well-formed (a
// non-nullable field would need a ctor or default initializer, §7.9).
// bz_string_release is null-safe. NOTE: kept to a SINGLE object — a `using
// var` object construction inside a tight loop trips a PRE-EXISTING
// (base-main) object-disposal leak unrelated to strings (the Holder object
// itself leaks even with an int field), which is out of #429's lane; a
// single object disposes correctly and exercises the string-field
// retain→release path cleanly.
//
// Migrated from a class-source `with`-expression store (former §22.11's
// object-initializer sugar, `new Holder() with Name := … end`) — §9.5
// (v0.188) retired that sugar for class sources; #1735.
const char* kObjectInitializerStore =
    "module M; class Holder "
    "  public Name: string?; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); h.Name := \"oi\" + \"block\"; "
    "  return h.Name!.Length; "              // "oiblock" => 7
    "end Main; end C;";

// §28.9: a string field whose value is a BORROW (a constructor PARAMETER that is
// itself a literal at the call site). The field RETAINS it (independent +1);
// disposal releases the field's reference, never the literal (a literal carries
// the sentinel rc → release is a no-op). LSan-clean, and crucially no
// double-free of the literal.
const char* kFieldStoreBorrowParamLoop =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor(n: string) this.Name := n; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(\"borrowed\"); "
    "    total := total + h.Name.Length; "
    "    i := i + 1; "
    "  end; "
    "  if total = 1600000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9: a string field on a class with an EXPLICIT destructor that ALSO reads
// the field (the destructor body runs before the auto field-release). The field
// must still be intact when the destructor reads it, then released after — no
// UAF in the destructor, no leak after. LSan-clean.
const char* kFieldWithDestructorReads =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"dt\" + \"or\"; end constructor; "
    "  destructor "
    "    var n: int := this.Name.Length; "   // read field in destructor body
    "  end destructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Name.Length; "
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// §9 (RECORD control): a record's string field is a BORROW — the record does not
// own it and has no disposer. So storing a +1 producer into a record string
// field leaves it conservatively LEAKED (as before #429), never double-freed.
// Value + no-corruption: the record reads back intact; a residual leak tolerated.
const char* kRecordStringFieldNoDoubleFree =
    "module M; record Box(Tag: string); "
    "class C public static method Main(): int "
    "  var b := new Box(\"re\" + \"cord\"); "
    "  return b.Tag.Length; "                // "record" => 6
    "end Main; end C;";

// §28.9 (cardinal UAF guard, HEAP value): a local aliases a field holding a HEAP
// (+1 producer) string, the field is then OVERWRITTEN (release-old), and the
// alias is read AFTER. The alias borrow-retained the heap value at bind (#429),
// so the overwrite's release-old only decrements the field's reference, never
// freeing the value the alias still reads — no UAF. (Before the borrow-retain
// fix this faulted with a heap-buffer-overflow / SEGV.) LSan-clean: the alias's
// own +1 is released at its scope end. Returns the ALIAS's length (the old value
// "initheap" => 8), read after the field overwrite.
const char* kAliasOverwriteHeapNoUaf =
    "module M; class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := \"init\" + \"heap\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  var alias: string := h.Name; "        // borrow-retain the heap field value
    "  h.Name := \"x\" + \"y\"; "            // overwrite (release-old)
    "  return alias.Length; "                // "initheap" => 8, intact
    "end Main; end C;";

// §28.4 / §28.9: INHERITANCE — base and derived each declare a string field; the
// base's is released by the base CORE disposer (emitBaseDisposeChain), the
// derived's by the derived disposer — once each, no double-release. Looped for
// steady-state LSan-clean reclaim.
const char* kInheritedStringFieldsLoop =
    "module M; class Base "
    "  public Tag: string; "
    "  public constructor() this.Tag := \"ba\" + \"se\"; end constructor; "
    "end Base; "
    "class Derived : Base "
    "  public Extra: string; "
    "  public constructor() this.Extra := \"ext\" + \"ra\"; end constructor; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    total := total + d.Tag.Length + d.Extra.Length; "  // 4 + 5
    "    i := i + 1; "
    "  end; "
    "  if total = 900000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.4 / §28.9: a class with BOTH an owned class field AND string fields, nested
// (the inner class also has a string field). Disposal cascades: outer releases
// its string field + disposes the owned inner (which releases ITS string field).
// The strongest end-to-end check that string-field release composes with the
// existing owned-field disposal cascade. Looped → steady-state LSan-clean.
const char* kOwnedFieldPlusStringCascadeLoop =
    "module M; class Inner "
    "  public Data: string; "
    "  public constructor() this.Data := \"in\" + \"ner\"; end constructor; "
    "end Inner; "
    "class Outer "
    "  public owned Child: Inner; "
    "  public Label: string; "
    "  public constructor() "
    "    this.Child := new Inner(); "
    "    this.Label := \"out\" + \"er\"; "
    "  end constructor; "
    "end Outer; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var o := new Outer(); "
    "    total := total + o.Label.Length + o.Child.Data.Length; "  // 5 + 5
    "    i := i + 1; "
    "  end; "
    "  if total = 1000000 then return 0; end; return 1; "
    "end Main; end C;";

// §4.7 / §28.9: null-coalescing assignment `field ??= e` into a (null) string?
// class field takes an independent +1 (transfer producer / retain borrow),
// released on disposal → reclaimed, LSan-clean. The store happens only when the
// field is null, so there is no old value to release.
const char* kFieldCoalesceAssign =
    "module M; class Holder "
    "  public Name: string?; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  h.Name ??= \"co\" + \"alesce\"; "
    "  return h.Name!.Length; "              // "coalesce" => 8
    "end Main; end C;";

// §7.7 (#1190): null-coalescing assignment `x ??= e` SHORT-CIRCUITS — the RHS is
// evaluated ONLY when x is currently null. A counting method `Bump()` runs once
// (for the null `d`), never for the non-null `c`. Pre-fix the RHS lowered
// eagerly and `Bump()` ran twice (Count == 2). Encodes Count*100 + c! *10 + d!:
// non-null `c` keeps 1, null `d` becomes 7, Count is 1 → 1*100 + 1*10 + 7 == 117.
const char* kCoalesceAssignShortCircuit =
    "module M; class C "
    "  public static Count: int := 0; "
    "  public static method Bump(): int "
    "    Count := Count + 1; return 7; "
    "  end Bump; "
    "  public static method Main(): int "
    "    var c: int? := 1; "
    "    c ??= Bump(); "            // non-null → Bump() must NOT run
    "    var d: int? := null; "
    "    d ??= Bump(); "            // null → Bump() runs once, d := 7
    "    return Count * 100 + c! * 10 + d!; "
    "  end Main; "
    "end C;";

// §7.7 / §28.9 (#1190): a non-null `string?` field `??=` a +1 STRING PRODUCER
// (`"co" + "alesce"`) must NOT evaluate the producer — short-circuit. Pre-fix the
// producer minted a +1 that was never stored (the field was non-null) nor
// released → a LEAK. The field keeps "kept" (length 4); LSan-clean confirms the
// producer was never minted.
const char* kCoalesceAssignNonNullNoLeak =
    "module M; class Holder "
    "  public Name: string?; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder(); "
    "  h.Name := \"kept\"; "
    "  h.Name ??= \"co\" + \"alesce\"; "   // non-null → producer skipped, no leak
    "  return h.Name!.Length; "            // "kept" => 4
    "end Main; end C;";

// §28.5 / §28.9: EXPLICIT `.Dispose()` (not `using var`) reclaims the string
// field too — the disposer's run-once flag + the post-release slot-nulling make a
// re-entrant disposal a no-op, so no double-free. Looped → steady-state
// LSan-clean.
const char* kExplicitDisposeLoop =
    "module M; class Holder : IDisposable "
    "  public Name: string; "
    "  public constructor() this.Name := \"ex\" + \"plicit\"; end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    var h := new Holder(); "
    "    total := total + h.Name.Length; "    // "explicit" => 8
    "    h.Dispose(); "
    "    i := i + 1; "
    "  end; "
    "  if total = 800000 then return 0; end; return 1; "
    "end Main; end C;";

// --- Programs: STRING RETURN escape (#429 RETURN category) -----------------
//
// Ownership convention (#429 RETURN sub-slice), STRING returned from a
// USER-DEFINED, LOCALLY-COMPILED method whose declared return type is `string`:
//   - The callee mints EXACTLY +1 for the returned value (§28.3 ownership
//     transfer): a +1 producer (`+`/interpolation/range-slice) is TRANSFERRED
//     (no retain); anything else (literal / param / field / call = a borrow) is
//     RETAINED, so the returned value carries exactly one reference to the
//     caller. A non-escaping returned LOCAL is excluded from the scope-end
//     string release so it is not freed before `ret`.
//   - The caller treats such a call result like any other +1 producer: a
//     `var s := Make()` local TRANSFERS the single reference (no extra retain)
//     and releases it at scope end iff the local is proven non-escaping → the
//     callee's +1 is balanced by the caller's release (no leak, no UAF). Stored
//     into a class string field, the field TRANSFERS the +1 (released on
//     disposal); passed onward as an argument it leaks the +1 (the ARGUMENT-pass
//     category is out of this sub-slice — safe, never a UAF/double-free).

// §28.9 / §28.3: a method returning a fresh `+` concat (a +1 producer). The
// callee TRANSFERS the producer reference out; the caller's non-escaping `var s`
// TRANSFERS it in and releases it at scope end → freed each iteration. Looped
// 200k for steady-state. LEAKED before this sub-slice (the caller never released
// the callee's +1).
const char* kReturnConcatLoopReclaimed =
    "module M; class C "
    "  public static method Build(): string return \"re\" + \"turn\"; end Build; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var s: string := C.Build(); "        // +1 from callee, released at scope end
    "      total := total + s.Length; "          // "return" => 6
    "      i := i + 1; "
    "    end; "
    "    if total = 1200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3: a method returning a string LITERAL. A literal is a sentinel
// (rc = 0x7FFF…): the callee's mint-retain is a no-op, and the caller's
// transfer-then-release is a no-op too. No leak (a literal is a constant global,
// not a heap allocation), no double-free of the literal. Looped for steady state.
const char* kReturnLiteralLoopClean =
    "module M; class C "
    "  public static method Lit(): string return \"lit\"; end Lit; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var s: string := C.Lit(); "
    "      total := total + s.Length; "          // "lit" => 3
    "      i := i + 1; "
    "    end; "
    "    if total = 600000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3: a method returning a borrowed FIELD `return this.Tag;`. The
// field value is a borrow, so the callee RETAINS it → the returned value carries
// an INDEPENDENT +1 (the field keeps its own +1, released on the owner's
// disposal). The caller's non-escaping `var s` transfers+releases the returned
// +1 → no UAF of the field's value, and no leak. The cardinal guard: the
// callee's retain is what keeps the returned value alive past the owner; without
// it the caller would read a value the owner could free. Looped → steady-state
// LSan-clean (the Holder + its field reclaimed each iteration via using var).
const char* kReturnBorrowedFieldLoopNoUaf =
    "module M; class Holder "
    "  public Tag: string; "
    "  public constructor() this.Tag := \"fie\" + \"ld\"; end constructor; "
    "  public method Get(): string return this.Tag; end Get; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var h := new Holder(); "
    "    var s: string := h.Get(); "             // callee retains the borrow → +1
    "    total := total + s.Length; "            // "field" => 5
    "    i := i + 1; "
    "  end; "
    "  if total = 500000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §28.3: a method returning a PARAM `return s;`. The param is a borrow,
// so the callee RETAINS it → the returned value carries +1. The caller's
// non-escaping local transfers+releases it. The call-site argument is a literal
// (sentinel), so the retain/release on it are no-ops and the literal is never
// freed. Looped → steady-state clean.
const char* kReturnParamLoopClean =
    "module M; class C "
    "  public static method Echo(s: string): string return s; end Echo; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var r: string := C.Echo(\"echoed\"); "
    "      total := total + r.Length; "          // "echoed" => 6
    "      i := i + 1; "
    "    end; "
    "    if total = 1200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3: return inside an IF / early-return. Both the early `return` and
// the fall-through `return` are direct returns of a +1 producer; each path mints
// +1 (transfers the producer). The caller's non-escaping local releases it.
// Exercises the per-path return mint, not just the single tail return. Looped.
const char* kReturnInIfEarlyReturnLoop =
    "module M; class C "
    "  public static method Pick(b: bool): string "
    "    if b then return \"ye\" + \"s\"; end; "  // early return of a +1 producer
    "    return \"n\" + \"o\"; "                  // tail return of a +1 producer
    "  end Pick; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var s: string := C.Pick(i = 0); "      // "no" for i>0 => length 2
    "      total := total + s.Length; "
    "      i := i + 1; "
    "    end; "
    "    if total = 400001 then return 0; end; return 1; " // "yes"(3) + 199999*"no"(2) = 400001
    "  end Main; "
    "end C;";

// §28.9 / §28.3 / §4.6: a returned call result stored DIRECTLY into a class
// string field — the field-store ARC composes with the RETURN mint. The call is
// a +1 producer (RETURN category), so lowerStringFieldStore TRANSFERS the +1
// into the field (no extra retain); disposal releases it → reclaimed. The
// strongest interplay check: RETURN mint hands off to field-store transfer with
// a single balanced +1. Looped → steady-state LSan-clean.
const char* kReturnIntoFieldStoreLoop =
    "module M; class Maker "
    "  public static method Make(): string return \"ma\" + \"de\"; end Make; "
    "end Maker; "
    "class Holder "
    "  public Name: string; "
    "  public constructor() this.Name := Maker.Make(); end constructor; " // transfer +1
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 200000 do "
    "    using var h := new Holder(); "
    "    total := total + h.Name.Length; "        // "made" => 4
    "    i := i + 1; "
    "  end; "
    "  if total = 800000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §28.3: `var t := <producer>; return t;` — a returned LOCAL that ALREADY
// owns a +1 (the `+` producer transferred its single reference into the slot).
// The return TRANSFERS that +1 (no second retain) — minting a fresh one would
// leak, since the escaped local is not released at scope end. The caller's
// non-escaping local transfers+releases the single +1 → exactly one allocation
// freed per iteration. Looped → steady-state LSan-clean. The decisive guard
// against double-counting the producer-local's +1 on return.
const char* kReturnProducerLocalLoopNoDouble =
    "module M; class C "
    "  public static method Build(): string "
    "    var t: string := \"lo\" + \"cal\"; "    // producer local owns +1
    "    return t; "                              // TRANSFER its +1 (no retain)
    "  end Build; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var s: string := C.Build(); "
    "      total := total + s.Length; "           // "local" => 5
    "      i := i + 1; "
    "    end; "
    "    if total = 1000000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.3: `var t := p; return t;` where `p` is a PARAM — exercises the
// borrow-local arm of returnedStringLocalOwnsPlusOne: a borrow local is RETAINED
// at bind (stringBorrowAll_) and so owns a +1, which the return TRANSFERS (no
// second retain). Returning it with a retain instead would mint an EXTRA +1 that
// leaks; transferring keeps the round trip balanced. The call-site argument is a
// literal (sentinel) so all retain/release on `p` are no-ops — the test is
// LSan-clean and verifies correctness + no UAF/double-free of the borrow-local
// return path. (A leak-free demonstration with a HEAP param argument is blocked
// by the ARGUMENT-pass category being out of this sub-slice: passing a heap
// string as an argument leaks the argument's +1 regardless of the return path.)
const char* kReturnBorrowLocalOfParamLoop =
    "module M; class C "
    "  public static method Wrap(p: string): string "
    "    var t: string := p; "                    // borrow local: retained +1 at bind
    "    return t; "                              // TRANSFER the retained +1
    "  end Wrap; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 200000 do "
    "      var r: string := C.Wrap(\"wrapped\"); "
    "      total := total + r.Length; "           // "wrapped" => 7
    "      i := i + 1; "
    "    end; "
    "    if total = 1400000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §31.2 / §28.9 / §28.3: `return <producer>;` inside a try-with-FINALLY. The +1
// mint runs before the value is stashed into the finally frame's return slot and
// routed through the finally block; the finally body runs, then the stashed value
// is ret'd. The returned +1 producer is a fresh allocation not aliased by any
// in-scope local, so the finally body's disposals never free it (no UAF), and the
// caller's transfer+release balances the +1 (no leak). Confirms the mint composes
// with the §31.2 return-through-finally path. Looped → steady-state LSan-clean.
const char* kReturnConcatThroughFinallyLoop =
    "module M; class C "
    "  public static method Build(): string "
    "    var guard: int := 0; "
    "    try "
    "      return \"fin\" + \"ally\"; "            // return through the finally
    "    finally "
    "      guard := guard + 1; "                  // finally body runs, no UAF
    "    end; "
    "  end Build; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 100000 do "
    "      var s: string := C.Build(); "
    "      total := total + s.Length; "            // "finally" => 7
    "      i := i + 1; "
    "    end; "
    "    if total = 700000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.4 / §28.9 / §28.3: a VIRTUAL/OVERRIDE string-returning method called
// through a base reference. Both the base virtual body and the derived override
// body go through the SAME ReturnStmt +1 minting, so the dynamic target hands
// back exactly +1 regardless of which override runs; the call site treats it as a
// +1 producer (the static callee is a declared-string MethodDecl). The caller's
// non-escaping local transfers+releases the single +1 → no leak, and critically
// no double-free across the vtable dispatch. Looped → steady-state LSan-clean.
const char* kReturnVirtualOverrideLoop =
    "module M; class Base "
    "  public virtual method Name(): string return \"ba\" + \"se\"; end Name; "
    "end Base; "
    "class Derived : Base "
    "  public override method Name(): string return \"deri\" + \"ved\"; end Name; "
    "end Derived; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var d := new Derived(); "
    "    var b: Base := d; "
    "    var s: string := b.Name(); "             // dynamic dispatch → "derived"
    "    total := total + s.Length; "             // 7
    "    i := i + 1; "
    "  end; "
    "  if total = 700000 then return 0; end; return 1; "
    "end Main; end C;";

// §11 / §28.9 / §28.3: a string-returning method called through an INTERFACE
// reference (itable dispatch). The interface method's MethodDecl declares `:
// string`, so the call site recognizes the +1-producer handoff; the implementing
// class's body mints +1 on return. Caller transfers+releases → no leak, no
// double-free across the itable dispatch. Looped → steady-state LSan-clean.
const char* kReturnInterfaceDispatchLoop =
    "module M; interface INamed "
    "  method Name(): string; "
    "end INamed; "
    "class Thing : INamed "
    "  public method Name(): string return \"th\" + \"ing\"; end Name; "
    "end Thing; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 100000 do "
    "    using var t := new Thing(); "
    "    var n: INamed := t; "
    "    var s: string := n.Name(); "             // itable dispatch → "thing"
    "    total := total + s.Length; "             // 5
    "    i := i + 1; "
    "  end; "
    "  if total = 500000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 / §28.3: nested call chain `f(g())`. `g()` (Build) mints +1; it is passed
// DIRECTLY as an ARGUMENT to `f` (Len) — the ARGUMENT-pass category, OUT of this
// RETURN sub-slice, so the inner +1 is conservatively LEAKED (never released by
// the caller — there is no bound local). The cardinal point here is memory
// SAFETY: the callee `Len` reads its borrowed param intact (no UAF), the result
// is correct, and there is no double-free. Value + no-corruption (a residual
// leak from the un-bound inner +1 is tolerated; this sub-slice does not claim the
// argument-pass category).
const char* kNestedCallChainNoUaf =
    "module M; class C "
    "  public static method Build(): string return \"ne\" + \"sted\"; end Build; "
    "  public static method Len(s: string): int return s.Length; end Len; "
    "  public static method Main(): int "
    "    return C.Len(C.Build()); "               // "nested" => 6
    "  end Main; "
    "end C;";

// --- Programs: COLLECTION-INSERT escape (#429 collection-insert sub-slice,
//     strings in a BORROWING Array<string> only) -----------------------------
//
// Ownership convention (#429 collection-insert sub-slice), a STRING inserted
// into a borrowing Array<string> (Stack/Queue<string> share the array backing):
//   - The array becomes a refcount CO-OWNER: it takes an INDEPENDENT +1 per
//     element at Add / Insert / indexer-set / array-literal (a +1 producer is
//     TRANSFERRED, anything else RETAINED — mirrors the field-store rule), and a
//     copy/materialize (Sorted/Reversed/ToArray/AddRange) retains the copied
//     elements so the new array independently co-owns its +1s.
//   - At the array's free/dispose, scope-end OR an owner's disposal, EACH element
//     is bz_string_release'd exactly once (the string-release element disposer);
//     Remove/RemoveAt/Clear release the affected element(s) too. The array's +1
//     is INDEPENDENT of the producer's local +1, so no double-free; a sentinel/
//     literal/null element is a no-op.
// NOTE: each producer uses a helper `Mk(s) => s + "!"` so the inserted string is
// a genuine HEAP allocation — a foldable literal-concat like `"a" + "b"` is a
// constant sentinel (never heap), which would not exercise the reclaim path.

// §28.9 / §28.11 (cardinal insert+free): produced strings inserted into a `using
// var` Array<string> are released at scope end. Looped 50 with a resize → the
// whole array (backing + every element) is reclaimed per iteration. Leaked
// before this sub-slice (the array dropped its elements without releasing them).
const char* kArrayInsertLoopReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var arr := new Array<string>(); "
    "      arr.Add(C.Mk(\"alpha\")); "             // "alpha!" => 6
    "      arr.Add(C.Mk(\"beta\")); "              // "beta!"  => 5
    "      total := total + arr.Get(0).Length + arr.Get(1).Length; "  // 11
    "      i := i + 1; "
    "    end; "
    "    if total = 550000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (resize stress): many produced strings forcing several array
// growths; on free every element is released exactly once → LSan-clean. Without
// the per-element release the 30 element strings would all leak each iteration.
const char* kArrayManyElemsResizeReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"#\"; end Mk; "
    "  public static method Main(): int "
    "    var it: int := 0; var total: int := 0; "
    "    while it < 20000 do "
    "      using var arr := new Array<string>(); "
    "      var k: int := 0; "
    "      while k < 30 do "
    "        arr.Add(C.Mk(\"node\")); "            // "node#" => 5
    "        k := k + 1; "
    "      end; "
    "      total := total + arr.Get(29).Length; "  // 5
    "      it := it + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 (insert then read back, no UAF): a produced string is inserted, read
// back through Get and used while the array is still alive → correct value, no
// UAF. The element carries the array's +1; reading it does not consume that +1.
const char* kArrayInsertReadBackNoUaf =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(C.Mk(\"hello\")); "              // "hello!" => 6
    "    var s: string := arr.Get(0); "          // borrow of the element
    "    return s.Length; "                       // 6, read while array alive
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (indexer overwrite): `arr[i] := other` releases the OLD element
// and takes a +1 on the NEW one — only the final value survives to free. Looped
// so a leaked OLD value (no release-old) OR a leaked array (no release-on-free)
// would accumulate; both are guarded. Self-overwrite-safe via retain-before-
// release. Returns the final element length.
const char* kArrayIndexerOverwriteReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    using var arr := new Array<string>(); "
    "    arr.Add(C.Mk(\"orig\")); "               // "orig!" initial
    "    while i < 50000 do "
    "      arr[0] := C.Mk(\"updated\"); "         // release-old, retain/transfer-new
    "      i := i + 1; "
    "    end; "
    "    return arr.Get(0).Length; "              // "updated!" => 8
    "  end Main; "
    "end C;";

// §28.9 (indexer overwrite, alias read — cardinal UAF guard): a local aliases
// the element, the slot is overwritten, then the alias is read. The alias must
// still see valid memory. With release-old only dropping the array's +1, and the
// alias holding the value through Get (the element still had the array's +1 when
// aliased and the array is alive), the overwrite must not free what the alias
// reads. Value + no-corruption (a residual leak is tolerated; a UAF is not).
const char* kArrayOverwriteAliasReadNoUaf =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"@\"; end Mk; "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(C.Mk(\"keepme\")); "             // "keepme@" => 7
    "    var alias: string := arr.Get(0); "       // alias the element
    "    arr[0] := C.Mk(\"replaced\"); "          // overwrite slot 0
    "    return alias.Length; "                    // 7, must be intact
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (Remove/Clear release): RemoveAt then Clear on a `using var`
// Array<string> release the affected elements (else they leak). Looped → any
// per-iteration leak would accumulate. After RemoveAt(0) and Clear() the array
// is empty; the array itself is freed at scope end. Returns 0.
const char* kArrayRemoveClearReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 50000 do "
    "      using var arr := new Array<string>(); "
    "      arr.Add(C.Mk(\"one\")); "
    "      arr.Add(C.Mk(\"two\")); "
    "      arr.Add(C.Mk(\"three\")); "
    "      arr.RemoveAt(0); "                      // release removed element
    "      arr.Clear(); "                          // release remaining elements
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (Remove(item) release): Remove by value releases the matched
// element. Looped; a per-iteration leak of the removed string would accumulate.
// The QUERY is a string LITERAL (a sentinel — never heap, no argument-pass leak,
// which is a separate sub-slice); §25.2 string Remove matches by CONTENT, so the
// literal "red!" matches the heap-produced element "red!" (= seed + "!").
const char* kArrayRemoveItemReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var arr := new Array<string>(); "
    "      arr.Add(C.Mk(\"red\")); "              // heap "red!" => 4
    "      arr.Add(C.Mk(\"green\")); "            // heap "green!" => 6
    "      var found: bool := arr.Remove(\"red!\"); "  // literal query, content-match
    "      total := total + arr.Get(0).Length; " // "green!" remains => 6
    "      i := i + 1; "
    "    end; "
    "    if total = 300000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (Insert + indexer-set composition): Insert at an index, then
// overwrite an existing slot. Both paths co-own/release correctly. Single-shot.
const char* kArrayInsertThenSet =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    using var arr := new Array<string>(); "
    "    arr.Add(C.Mk(\"aa\")); "
    "    arr.Insert(0, C.Mk(\"bbb\")); "           // "bbb!" at index 0
    "    arr[1] := C.Mk(\"cccc\"); "              // overwrite index 1 (was "aa!")
    "    return arr.Get(0).Length + arr.Get(1).Length; "  // 4 + 5 = 9
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (copy producer — the double-free guard): Sorted() shallow-copies
// the source array's element pointers. The copy independently co-owns a +1 per
// element (retained at copy), so disposing BOTH the source and the sorted copy
// releases each string exactly twice over its two co-owners — never freeing a
// value the other still holds (no double-free) and leaving no leak. Without the
// copy-retain the sorted copy would release un-retained pointers, prematurely
// freeing the source's strings (a double-free). Both are `using var`.
const char* kArraySortedCopyNoDoubleFree =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    using var src := new Array<string>(); "
    "    src.Add(C.Mk(\"banana\")); "
    "    src.Add(C.Mk(\"apple\")); "
    "    src.Add(C.Mk(\"cherry\")); "
    "    using var srt := src.Sorted(); "          // shallow copy, retained
    "    return srt.Get(0).Length + src.Get(0).Length; "  // "apple!"(6)+"banana!"(7)=13
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (AddRange — co-own, no double-free): AddRange copies the source
// elements into a second array; the destination takes an INDEPENDENT +1 per
// element. Disposing both arrays releases each string over its two co-owners —
// no premature free of a value the other still holds. Both `using var`.
const char* kArrayAddRangeNoDoubleFree =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    using var a := new Array<string>(); "
    "    a.Add(C.Mk(\"xx\")); "                    // "xx!" => 3
    "    a.Add(C.Mk(\"yy\")); "                    // "yy!" => 3
    "    using var b := new Array<string>(); "
    "    b.AddRange(a); "                           // co-own a's elements
    "    return a.Get(0).Length + b.Get(1).Length; "  // 3 + 3 = 6
    "  end Main; "
    "end C;";

// §28.9 / §28.11 (RETURN-escape interplay — no double-free): an element read out
// of the array AND returned/used after the array is gone. arr.Get(0) returns a
// borrow; binding it to a `var` borrow-retains it (#429), so the returned value
// carries its OWN +1 — when the array is freed (releasing its +1) the returned
// value survives, and is reclaimed at the caller's scope. No double-free across
// the array-free / return handoff. Value + leak-free.
const char* kArrayElementReturnInterplay =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Pick(): string "
    "    using var arr := new Array<string>(); "
    "    arr.Add(C.Mk(\"chosen\")); "             // "chosen!" => 7
    "    var e: string := arr.Get(0); "           // borrow-retained at bind (+1)
    "    return e; "                               // transfer its +1 to the caller
    "  end Pick; "                                 // arr freed here: releases its +1
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      var s: string := C.Pick(); "            // caller transfers+releases +1
    "      total := total + s.Length; "            // 7
    "      i := i + 1; "
    "    end; "
    "    if total = 350000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.4 / §28.9 / §28.11 (owned field of Array<string>): a class with an `owned
// Array<string>` field populated in the constructor with a HEAP-produced string;
// on the owner's disposal the array's backing is freed AND each element released
// (emitOwnedFieldDisposals). Looped → steady-state LSan-clean (the object + its
// array + every element reclaimed each iteration). The constructor receives a
// param and stores `param + "!"` (a +1 producer transferred into the array).
const char* kOwnedArrayStringFieldReclaimed =
    "module M; class Holder "
    "  public owned Tags: Array<string>; "
    "  public constructor(seed: string) "
    "    this.Tags := new Array<string>(); "
    "    this.Tags.Add(seed + \"!\"); "           // +1 producer transferred in
    "    this.Tags.Add(seed + \"?\"); "
    "  end constructor; "
    "end Holder; "
    "class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var h := new Holder(\"tag\"); "     // "tag!"(4) + "tag?"(4)
    "    total := total + h.Tags.Get(0).Length + h.Tags.Get(1).Length; "  // 8
    "    i := i + 1; "
    "  end; "
    "  if total = 400000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 (CONTROL — Array<int> unaffected): a scalar-element array is never given
// a string disposer; insert/free is byte-only, no release loop. Looped to prove
// the scalar path is untouched (no spurious release of an inttoptr'd scalar).
const char* kArrayIntControlUnaffected =
    "module M; class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var arr := new Array<int>(); "
    "    arr.Add(7); arr.Add(35); "
    "    total := total + arr.Get(0) + arr.Get(1); "  // 42
    "    i := i + 1; "
    "  end; "
    "  if total = 2100000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs: Set<string> element reference-counting (#429 set category) ---

// §28.9 / §4.10: heap strings inserted into a `using var Set<string>` are
// retained on Add and released when the set is freed at scope end.  Looped so
// any per-iteration leak accumulates.  The `Mk` helper forces a heap string
// (a +1 producer; the set transfers the +1 and co-owns it).
const char* kSetStringInsertDispose =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var s := new Set<string>(); "
    "      s.Add(C.Mk(\"alpha\")); "
    "      s.Add(C.Mk(\"beta\")); "
    "      total := total + s.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.10 / §28.11 (Remove releases): Set<string>.Remove releases the
// element's +1 (else the removed string leaks).  Looped.
const char* kSetStringRemoveReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var s := new Set<string>(); "
    "      s.Add(C.Mk(\"red\")); "
    "      s.Add(C.Mk(\"green\")); "
    "      var ok: bool := s.Remove(\"red!\"); "
    "      total := total + s.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 50000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.10 / §28.11 (Clear releases): Set<string>.Clear releases every
// element's +1 (else all cleared strings leak).  Looped.
const char* kSetStringClearReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 50000 do "
    "      using var s := new Set<string>(); "
    "      s.Add(C.Mk(\"one\")); "
    "      s.Add(C.Mk(\"two\")); "
    "      s.Add(C.Mk(\"three\")); "
    "      s.Clear(); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §28.9 / §4.10: many elements trigger a rehash; all must be released on free.
const char* kSetStringManyRehashFree =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"#\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 10000 do "
    "      using var s := new Set<string>(); "
    "      s.Add(C.Mk(\"a\")); s.Add(C.Mk(\"b\")); s.Add(C.Mk(\"c\")); "
    "      s.Add(C.Mk(\"d\")); s.Add(C.Mk(\"e\")); s.Add(C.Mk(\"f\")); "
    "      s.Add(C.Mk(\"g\")); s.Add(C.Mk(\"h\")); s.Add(C.Mk(\"i\")); "
    "      s.Add(C.Mk(\"j\")); s.Add(C.Mk(\"k\")); s.Add(C.Mk(\"l\")); "
    "      s.Add(C.Mk(\"m\")); s.Add(C.Mk(\"n\")); s.Add(C.Mk(\"o\")); "
    "      s.Add(C.Mk(\"p\")); s.Add(C.Mk(\"q\")); s.Add(C.Mk(\"r\")); "
    "      s.Add(C.Mk(\"s\")); s.Add(C.Mk(\"t\")); "
    "      total := total + s.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 200000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 (CONTROL — Set<int> unaffected): integer-element sets store no heap
// strings; the new string-release path must never touch them.
const char* kSetIntControlUnaffected =
    "module M; class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var s := new Set<int>(); "
    "    s.Add(3); s.Add(7); s.Add(3); "
    "    total := total + s.Count; "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// ---- Programs: Dictionary<K,string> value reference-counting (#429) ---------

// §28.9 / §4.8: heap strings stored as values in a string-keyed Dictionary are
// retained on Add and released when the dict is freed at scope end.
const char* kDictStringValueInsertDispose =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var d := new Dictionary<string, string>(); "
    "      d.Add(\"k1\", C.Mk(\"hello\")); "
    "      d.Add(\"k2\", C.Mk(\"world\")); "
    "      total := total + d.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.8: generic-key Dictionary<int, string> retains the string values
// and releases them at scope-end free.
const char* kDictIntKeyStringValueInsertDispose =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var d := new Dictionary<int, string>(); "
    "      d.Add(1, C.Mk(\"foo\")); "
    "      d.Add(2, C.Mk(\"bar\")); "
    "      total := total + d.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.8 (overwrite — release old, retain new): `d[k] := newStr` must
// release the displaced old value and co-own the new one.  Looped.
const char* kDictStringValueOverwriteReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var d := new Dictionary<string, string>(); "
    "      d.Add(\"key\", C.Mk(\"first\")); "
    "      d[\"key\"] := C.Mk(\"second\"); "
    "      var v: string := d[\"key\"]; "
    "      total := total + v.Length; "
    "      i := i + 1; "
    "    end; "
    "    if total = 350000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.8 / §28.11 (Remove releases): Dictionary<string,string>.Remove
// releases the removed value's +1.  Looped.
const char* kDictStringValueRemoveReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      using var d := new Dictionary<string, string>(); "
    "      d.Add(\"a\", C.Mk(\"one\")); "
    "      d.Add(\"b\", C.Mk(\"two\")); "
    "      var ok: bool := d.Remove(\"a\"); "
    "      total := total + d.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 50000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

// §28.9 / §4.8 / §28.11 (Clear releases): Dictionary<string,string>.Clear
// releases every surviving value's +1.  Looped.
const char* kDictStringValueClearReclaimed =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"!\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; "
    "    while i < 50000 do "
    "      using var d := new Dictionary<string, string>(); "
    "      d.Add(\"x\", C.Mk(\"uno\")); "
    "      d.Add(\"y\", C.Mk(\"dos\")); "
    "      d.Add(\"z\", C.Mk(\"tres\")); "
    "      d.Clear(); "
    "      i := i + 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

// §28.9 (CONTROL — Dictionary<string,int> unaffected): scalar values are never
// given a string release disposer — the scalar encode/decode path is untouched.
const char* kDictStringKeyIntValueControlUnaffected =
    "module M; class C public static method Main(): int "
    "  var i: int := 0; var total: int := 0; "
    "  while i < 50000 do "
    "    using var d := new Dictionary<string, int>(); "
    "    d.Add(\"a\", 3); d.Add(\"b\", 7); "
    "    total := total + d.Count; "
    "    i := i + 1; "
    "  end; "
    "  if total = 100000 then return 0; end; return 1; "
    "end Main; end C;";

// §28.9 (cross-collection double-free guard): a heap string inserted into BOTH
// a Set<string> AND an Array<string> must be independently retained by each
// co-owner; disposing both must release each co-owner's +1 exactly once — no
// double-free, no leak.  The element is a borrow (identifier), so both
// collections RETAIN it; after both are freed the string is released twice
// (rc drains to the producer's original +1, released at local scope end).
const char* kSetAndArrayShareNoDoubleFree =
    "module M; class C "
    "  public static method Mk(s: string): string return s + \"@\"; end Mk; "
    "  public static method Main(): int "
    "    var i: int := 0; var total: int := 0; "
    "    while i < 50000 do "
    "      var shared: string := C.Mk(\"shared\"); "
    "      using var arr := new Array<string>(); "
    "      using var s := new Set<string>(); "
    "      arr.Add(shared); "
    "      s.Add(shared); "
    "      total := total + s.Count + arr.Count; "
    "      i := i + 1; "
    "    end; "
    "    if total = 100000 then return 0; end; return 1; "
    "  end Main; "
    "end C;";

}  // namespace

// --- LSan-clean tests (#429 field-store reclaim: leaked before, clean now) ---

BEZEL_TEST(codegen_escaping_string_429, field_store_ctor_producer_loop_reclaimed_28_9) {
    expectValueAndLeakFree(kFieldStoreCtorProducerLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, field_default_init_producer_loop_reclaimed_28_9) {
    expectValueAndLeakFree(kFieldDefaultInitProducerLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, auto_property_store_loop_reclaimed_28_9) {
    expectValueAndLeakFree(kAutoPropertyStoreLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, field_reassign_loop_bounded_no_uaf_28_9) {
    expectValueAndLeakFree(kFieldReassignLoop, 2);
}

BEZEL_TEST(codegen_escaping_string_429, local_self_assign_no_double_free_28_9) {
    expectValueAndNoCorruption(kLocalSelfAssign, 4);
}

BEZEL_TEST(codegen_escaping_string_429, field_assign_after_construction_store_reclaimed) {
    expectValueAndLeakFree(kObjectInitializerStore, 7);
}

BEZEL_TEST(codegen_escaping_string_429, field_store_borrow_param_loop_reclaimed_28_9) {
    expectValueAndLeakFree(kFieldStoreBorrowParamLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, field_with_destructor_reads_no_uaf_28_9) {
    expectValueAndLeakFree(kFieldWithDestructorReads, 0);
}

// --- Cardinal memory-safety guards (no UAF / double-free) -----------------

BEZEL_TEST(codegen_escaping_string_429, field_overwrite_alias_read_no_uaf_28_9) {
    // The alias borrow-retains the field value; overwriting the field releases
    // only the field's reference, never the value the alias still reads — no UAF.
    expectValueAndNoCorruption(kFieldOverwriteAliasReadNoUaf, 6);
}

// HEAP-value alias-overwrite: the decisive UAF guard (SEGV'd before the
// borrow-retain fix). The alias holds its own +1, so the field overwrite's
// release-old never frees what it reads, and the alias's +1 is reclaimed at
// scope end → fully LSan-clean.
BEZEL_TEST(codegen_escaping_string_429, alias_overwrite_heap_no_uaf_clean_28_9) {
    expectValueAndLeakFree(kAliasOverwriteHeapNoUaf, 8);
}

// --- Disposal composition: inheritance, owned-field cascade, explicit Dispose

BEZEL_TEST(codegen_escaping_string_429, inherited_string_fields_loop_reclaimed_28_4) {
    expectValueAndLeakFree(kInheritedStringFieldsLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, owned_field_plus_string_cascade_loop_28_4) {
    expectValueAndLeakFree(kOwnedFieldPlusStringCascadeLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, explicit_dispose_loop_reclaimed_28_5) {
    expectValueAndLeakFree(kExplicitDisposeLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, field_coalesce_assign_reclaimed_4_7) {
    expectValueAndLeakFree(kFieldCoalesceAssign, 8);
}

// §7.7 (#1190): `??=` short-circuits — the RHS runs only when the LHS is null.
BEZEL_TEST(codegen_escaping_string_429, coalesce_assign_short_circuits_nonnull_lhs_1190) {
    BEZEL_EXPECT_EQ(compileAndRun(kCoalesceAssignShortCircuit), 117);
}

// §7.7 / §28.9 (#1190): a non-null `string?` field `??=` a +1 producer must not
// evaluate (mint) the producer — leak-free confirms the short-circuit.
BEZEL_TEST(codegen_escaping_string_429, coalesce_assign_nonnull_skips_owned_rhs_1190) {
    expectValueAndLeakFree(kCoalesceAssignNonNullNoLeak, 4);
}

// --- RECORD control: borrow semantics, no double-free ---------------------

BEZEL_TEST(codegen_escaping_string_429, record_string_field_no_double_free_9) {
    expectValueAndNoCorruption(kRecordStringFieldNoDoubleFree, 6);
}

// --- RETURN escape category (#429 RETURN sub-slice): callee mints +1, caller
//     transfers + releases. Leaked before this sub-slice; clean now. ----------

BEZEL_TEST(codegen_escaping_string_429, return_concat_loop_reclaimed_28_3) {
    expectValueAndLeakFree(kReturnConcatLoopReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_literal_loop_clean_28_9) {
    expectValueAndLeakFree(kReturnLiteralLoopClean, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_borrowed_field_loop_no_uaf_28_3) {
    // The callee retains the borrowed field → the returned value carries an
    // independent +1, so the caller never reads a value the owner can free.
    expectValueAndLeakFree(kReturnBorrowedFieldLoopNoUaf, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_param_loop_clean_28_3) {
    expectValueAndLeakFree(kReturnParamLoopClean, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_in_if_early_return_loop_28_3) {
    expectValueAndLeakFree(kReturnInIfEarlyReturnLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_into_field_store_loop_28_3) {
    // RETURN mint composes with field-store transfer: a single balanced +1.
    expectValueAndLeakFree(kReturnIntoFieldStoreLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_producer_local_loop_no_double_28_3) {
    // Returning a producer LOCAL transfers its single +1 (no double-count).
    expectValueAndLeakFree(kReturnProducerLocalLoopNoDouble, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_borrow_local_of_param_loop_28_3) {
    // Returning a borrow LOCAL transfers its retained +1.
    expectValueAndLeakFree(kReturnBorrowLocalOfParamLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_concat_through_finally_loop_31_2) {
    // The +1 mint composes with the §31.2 return-through-finally path: no UAF
    // (the finally body cannot free the fresh returned +1), no leak.
    expectValueAndLeakFree(kReturnConcatThroughFinallyLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_virtual_override_loop_28_4) {
    // Both override bodies mint +1 on return → no double-free across vtable.
    expectValueAndLeakFree(kReturnVirtualOverrideLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, return_interface_dispatch_loop_11) {
    // Itable dispatch: the impl body mints +1; caller balances → clean.
    expectValueAndLeakFree(kReturnInterfaceDispatchLoop, 0);
}

BEZEL_TEST(codegen_escaping_string_429, nested_call_chain_no_uaf_28_3) {
    // f(g()): the inner +1 is passed as an ARGUMENT (out of the RETURN lane) and
    // is conservatively leaked; the cardinal point is no UAF / double-free.
    expectValueAndNoCorruption(kNestedCallChainNoUaf, 6);
}

// --- COLLECTION-INSERT escape category (#429 collection-insert sub-slice:
//     borrowing Array<string> co-owns +1 per element; leaked before, clean now) -

BEZEL_TEST(codegen_escaping_string_429, array_insert_loop_reclaimed_28_11) {
    // Produced strings inserted into a using-var Array<string> are released at
    // scope end (insert+free). Leaked before this sub-slice.
    expectValueAndLeakFree(kArrayInsertLoopReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, array_many_elems_resize_reclaimed_28_11) {
    // Resize stress: every element released on free → no leak across growth.
    expectValueAndLeakFree(kArrayManyElemsResizeReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, array_insert_read_back_no_uaf_28_9) {
    // Read an element back through Get and use it while the array is alive — the
    // element carries the array's +1, reading does not consume it (no UAF).
    expectValueAndLeakFree(kArrayInsertReadBackNoUaf, 6);
}

BEZEL_TEST(codegen_escaping_string_429, array_indexer_overwrite_reclaimed_28_11) {
    // arr[i] := other releases the OLD element and co-owns the NEW; only the
    // final value survives to free. Looped → bounded, no leak (retain-before-
    // release keeps it self-overwrite-safe).
    expectValueAndLeakFree(kArrayIndexerOverwriteReclaimed, 8);
}

BEZEL_TEST(codegen_escaping_string_429, array_overwrite_alias_read_no_uaf_28_9) {
    // The alias holds the element through Get; overwriting the slot releases only
    // the array's +1, never the value the alias still reads — no UAF.
    expectValueAndNoCorruption(kArrayOverwriteAliasReadNoUaf, 7);
}

BEZEL_TEST(codegen_escaping_string_429, array_remove_clear_reclaimed_28_11) {
    // RemoveAt + Clear release the affected elements (else they leak); looped.
    expectValueAndLeakFree(kArrayRemoveClearReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, array_remove_item_reclaimed_28_11) {
    // Remove(item) releases the matched element; looped → no per-iter leak.
    expectValueAndLeakFree(kArrayRemoveItemReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, array_insert_then_set_28_11) {
    // Insert + indexer-set compose: both co-own/release correctly.
    expectValueAndLeakFree(kArrayInsertThenSet, 9);
}

BEZEL_TEST(codegen_escaping_string_429, array_sorted_copy_no_double_free_28_11) {
    // The cardinal double-free guard: a Sorted() shallow copy co-owns a +1 per
    // element (retained at copy), so disposing both src and the copy never frees
    // a value the other still holds.
    expectValueAndLeakFree(kArraySortedCopyNoDoubleFree, 13);
}

BEZEL_TEST(codegen_escaping_string_429, array_addrange_no_double_free_28_11) {
    // AddRange co-owns the source elements with an independent +1 — no premature
    // free when both arrays are disposed.
    expectValueAndLeakFree(kArrayAddRangeNoDoubleFree, 6);
}

BEZEL_TEST(codegen_escaping_string_429, array_element_return_interplay_28_9) {
    // An element read out AND returned/used after the array is freed: the
    // borrow-retained return value survives the array's per-element release — no
    // double-free across the array-free / return handoff.
    expectValueAndLeakFree(kArrayElementReturnInterplay, 0);
}

BEZEL_TEST(codegen_escaping_string_429, owned_array_string_field_reclaimed_28_4) {
    // An owned Array<string> field releases each element on the owner's disposal.
    expectValueAndLeakFree(kOwnedArrayStringFieldReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, array_int_control_unaffected_28_9) {
    // CONTROL: a scalar Array<int> is never given a string disposer — the scalar
    // insert/free path is untouched (no spurious release of an inttoptr'd value).
    expectValueAndLeakFree(kArrayIntControlUnaffected, 0);
}

// --- LSan-clean tests (#429 Set<string> element category) --------------------

BEZEL_TEST(codegen_escaping_string_429, set_string_insert_dispose_reclaimed_28_9) {
    // Set<string>.Add retains each heap string; free releases them — no leak.
    expectValueAndLeakFree(kSetStringInsertDispose, 0);
}

BEZEL_TEST(codegen_escaping_string_429, set_string_remove_reclaimed_28_11) {
    // Set<string>.Remove releases the removed element's +1 — no leak.
    expectValueAndLeakFree(kSetStringRemoveReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, set_string_clear_reclaimed_28_11) {
    // Set<string>.Clear releases all elements' +1 — no leak.
    expectValueAndLeakFree(kSetStringClearReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, set_string_many_rehash_free_28_9) {
    // Many-element Set<string> (triggers rehash); all elements released on free.
    expectValueAndLeakFree(kSetStringManyRehashFree, 0);
}

BEZEL_TEST(codegen_escaping_string_429, set_int_control_unaffected_28_9) {
    // CONTROL: Set<int> is never given a string-release key-dispose path. Set<T>
    // for non-string T has a pre-existing key-copy leak (bz_dict_set_gen malloc'd
    // key bytes are not released at free — a separate correctness fix outside the
    // #429 scope). Check no UAF/double-free; tolerate the pre-existing leak.
    expectValueAndNoCorruption(kSetIntControlUnaffected, 0);
}

// --- LSan-clean tests (#429 Dictionary<K,string> value category) -------------

BEZEL_TEST(codegen_escaping_string_429, dict_str_key_string_value_insert_dispose_28_9) {
    // Dictionary<string,string> values retained on Add; released on dict free.
    expectValueAndLeakFree(kDictStringValueInsertDispose, 0);
}

BEZEL_TEST(codegen_escaping_string_429, dict_int_key_string_value_insert_dispose_28_9) {
    // Dictionary<int,string> values retained on Add; released on dict free.
    expectValueAndLeakFree(kDictIntKeyStringValueInsertDispose, 0);
}

BEZEL_TEST(codegen_escaping_string_429, dict_string_value_overwrite_reclaimed_28_9) {
    // Indexer-set on an existing key releases old value, retains new — no leak.
    expectValueAndLeakFree(kDictStringValueOverwriteReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, dict_string_value_remove_reclaimed_28_11) {
    // Dictionary<string,string>.Remove releases the removed value — no leak.
    expectValueAndLeakFree(kDictStringValueRemoveReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, dict_string_value_clear_reclaimed_28_11) {
    // Dictionary<string,string>.Clear releases all surviving values — no leak.
    expectValueAndLeakFree(kDictStringValueClearReclaimed, 0);
}

BEZEL_TEST(codegen_escaping_string_429, dict_scalar_value_control_unaffected_28_9) {
    // CONTROL: Dictionary<string,int> values are never given a string disposer.
    expectValueAndLeakFree(kDictStringKeyIntValueControlUnaffected, 0);
}

// --- Cross-collection double-free guard --------------------------------------

BEZEL_TEST(codegen_escaping_string_429, set_and_array_share_no_double_free_28_9) {
    // A string in both Set<string> and Array<string>: each independently retains
    // once; disposing both releases each co-owner exactly once — no double-free.
    // The `var shared` local holds its own +1 which the ownership analysis
    // considers escaping (pre-existing #422 scope — its residual +1 is a known
    // safe leak); the CRITICAL check here is no UAF/double-free.
    expectValueAndNoCorruption(kSetAndArrayShareNoDoubleFree, 0);
}
