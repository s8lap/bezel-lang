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
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver pipeline and
// run it; returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_429_" + std::to_string(counter++);
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
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_429_san_" + std::to_string(counter++);
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

// §28.9 / §22.11: a class OBJECT INITIALIZER block `new Holder() with Name := …`
// stores into a string field with retain/transfer, released on the (single)
// object's disposal → the field string is reclaimed, LSan-clean. The field is
// `string?` so the object-initializer form is well-formed (a non-nullable field
// would need a ctor or default initializer, §7.9). bz_string_release is
// null-safe. NOTE: kept to a SINGLE object — a `using var ... with ... end`
// object-initializer inside a tight loop trips a PRE-EXISTING (base-main)
// object-disposal leak unrelated to strings (the Holder object itself leaks even
// with an int field), which is out of #429's lane; a single object disposes
// correctly and exercises the string-field retain→release path cleanly.
const char* kObjectInitializerStore =
    "module M; class Holder "
    "  public Name: string?; "
    "end Holder; "
    "class C public static method Main(): int "
    "  using var h := new Holder() with Name := \"oi\" + \"block\" end; "
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

BEZEL_TEST(codegen_escaping_string_429, object_initializer_store_reclaimed_22_11) {
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

// --- RECORD control: borrow semantics, no double-free ---------------------

BEZEL_TEST(codegen_escaping_string_429, record_string_field_no_double_free_9) {
    expectValueAndNoCorruption(kRecordStringFieldNoDoubleFree, 6);
}
