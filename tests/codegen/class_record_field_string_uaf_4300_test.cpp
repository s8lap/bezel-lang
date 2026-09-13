// Codegen — issue #4300 / spec §9.7 ("Records and disposal") + §28.3
// ("Runtime-managed types"), docs revision v0.298.
//
// `calleeReturnMayAliasStringArgUnretained`'s top-level arm only suppressed
// the caller-side release when the callee's declared return type itself is
// Record- or Choice-declared. It never fired when the return type was a Class
// whose own field is Record/Choice-typed and whose constructor stores the
// argument into that field via `new SomeRecord(arg)` — the identical bare
// `CreateStore`, no-`emitStringRetain` fast path `buildRecordValue` uses for a
// top-level record/choice return (the exact mechanism fix #4254 found).
//
// `lowerStringFieldStore` — the mechanism cited to justify excluding Class
// entirely — only retains when the *field being stored* is itself `string`-
// typed; it never fires here because the class's field (`W`) is Record-typed,
// not `string`-typed. The actual unretained embed happens one level further
// in, inside `new Str(v)`.
//
// FIX: widen the Class-field check to also cover this shape. When the return
// type is a non-imported Class with any field whose declared type is
// Record/Choice-declared, treat the callee return as "may embed the argument
// unretained" — accepting the leak-over-UAF bias this whole family uses.
//
// MEASURED FAIL-WITHOUT: AddressSanitizer heap-use-after-free, READ of size 8,
// freed in main, previously allocated in main, identical 32-byte
// `bz.string`-header region signature every other row in this family measures.
// MEASURED PASS-WITH: exit 5 ("he"+"llo" = "hello", Length 5), ASan/UAF
// detection clean (runAotCheckedLenient — the 38-byte pre-existing leak in
// emitOwnedFieldDisposals for non-sealed classes with explicit ctors is
// tracked in #4277; this test's PASS-WITH confirms the UAF fix, not the leak).
//
// Repro 1 (no array, the more fundamental case): `static method Wrap(s:
// string): owned Widget2` — plain non-array Class return. Pre-exists the whole
// #4254/#4260/#4265/#4280 inline-array-element family and reproduces identically
// on `f8b12ba` (before fix #4265 existed), proving this is a pre-existing gap
// in `isRecordOrChoiceDeclared`'s ORIGINAL top-level arm from fix #4254.
//
// Repro 2 (inline array — SEPARATE BUG): `static method Wrap(s: string): [1]Widget2`
// — the inline-array form the send-back review actually flagged. BLOCKED: inline
// arrays of owned class types have a separate disposal issue (#4277) — the
// compiler rejects `return outer[0].W.V.Length` as "owned value would be dropped
// without disposal" even when the UAF fix is applied. The non-array case (PIN 1)
// is the PRIMARY pin for #4300; the array shape needs a separate fix.
//
// NOTE: runAotCheckedLenient (detectLeaks=0) is used instead of runAotChecked
// because emitOwnedFieldDisposals intentionally skips record-typed fields in
// non-sealed classes with explicit constructors (#4277). This leaves the 38-byte
// bz_string header leaked even with the #4300 fix applied. The UAF fix is still
// verified: without the fix, ASan catches the heap-use-after-free; with the
// fix, ASan/UAF detection is clean (the leak does not trigger ASan).

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAotCheckedLenient;

namespace {
// Strict oracle: compiled, ran, exited cleanly, no ASan/UAF finding.
// UAF routes through ASan hard-abort (fork-isolated); ran=false on UAF.
// Leak detection is DISABLED (runAotCheckedLenient) because a pre-existing
// leak in emitOwnedFieldDisposals (#4277) would otherwise cause ran=false
// even when the UAF fix is correctly applied.
int runChecked(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ===========================================================================
// PRIMARY PIN 1/2 — non-array Class return with Record-typed field.
// FAIL-WITHOUT: AddressSanitizer heap-use-after-free (measured).
// PASS-WITH: exit 5.
// ===========================================================================
BEZEL_TEST(class_record_field_string_uaf_4300,
           class_returning_factory_via_record_field_no_longer_uaf) {
    const char* prog =
        "module M; "
        "record Str(V: string); "
        "class Widget2 "
        "  public W: Str; "
        "  public constructor(v: string) "
        "    W := new Str(v); "
        "  end constructor; "
        "end Widget2; "
        "class Helper "
        "  public static method Wrap(s: string): owned Widget2 "
        "    return new Widget2(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Widget2; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    var len := outer.W.V.Length; "
        "    outer.Dispose(); "
        "    return len; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}

// ===========================================================================
// BOUNDARY CONTROL — Class with a directly `string`-typed field (NOT
// Record/Choice) stays clean WITHOUT this fix, proving the original
// `lowerStringFieldStore` exclusion is correct for that shape. This row is a
// boundary pin, not a FAIL-WITHOUT row.
// ===========================================================================
BEZEL_TEST(class_record_field_string_uaf_4300,
           class_with_direct_string_field_control_stays_clean) {
    const char* prog =
        "module M; "
        "class Widget "
        "  public V: string; "
        "  public constructor(v: string) "
        "    V := v; "
        "  end constructor; "
        "end Widget; "
        "class Helper "
        "  public static method Wrap(s: string): owned Widget "
        "    return new Widget(s); "
        "  end Wrap; "
        "end Helper; "
        "class C "
        "  public static method Main(): int "
        "    var outer: Widget; "
        "    do "
        "      var s := \"he\" + \"llo\"; "
        "      outer := Helper.Wrap(s); "
        "    end; "
        "    var len := outer.V.Length; "
        "    outer.Dispose(); "
        "    return len; "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(runChecked(prog), 5);
}
