// Codegen — a string stored into an IMPORTED class's `Object`-typed field is
// not retained: the source's scope-end release frees the string under the
// field, and the later read is a use-after-free. §28.9 (runtime-managed
// types), §4.6, §8.6, §17.4 (`as` is a borrow of its operand), §2.5 / §2.8.2
// (the imported interface), v0.355.
//
// THE DEFECT (issue #8306). The #8214 fix retains a string stored into a
// class field whose DECLARED type is `Object`/interface, so the field holds
// its own +1 and the source's scope-end release no longer frees the string
// under it. The classification that admits the field to the retain set
// (`Emitter::collectObjectStringStorageFields`) walked `ctx_.symbols()` —
// THIS module's symbols only — so a DEPENDENCY's public `Object`-typed field
// was never in the set, the store site did not retain, and the string was
// freed under the field.
//
// MEASURED on the base (`74c9d282`), with the library module `Lib` and the
// importer `App` below:
//
//   plain run    → `free(): invalid pointer`, rc 134
//   ASan run     → `ERROR: AddressSanitizer: heap-use-after-free … READ … in
//                   main` (the read is `h.O.ToString()` after the `do` block
//                   that owned the string ended)
//
// The SINGLE-MODULE twin of the same program is clean apart from the
// DOCUMENTED #8214 leak (the field's +1 — nothing releases it, because an
// `Object` slot may also hold a class instance and no runtime discriminator
// says which: #8215 / #5059). That leak is the oracle-live positive control
// below: it is what proves the sanitizer really is attached.
//
// THE FIX. `collectObjectStringStorageFields` now also walks the IMPORTED
// classes (`ctx_.importedSymbols()`), so a dependency's `Object`/interface-
// typed instance field is admitted to the same retain set. The field's kind
// comes from the dependency's OWN metadata — `ModuleMetadataBuilder` writes
// every exported member's declared type into the dependency's `.bezel.meta`
// as a `MetaType`, and `ModuleLoader` rebuilds the member and parks the
// resolved `sema::TypeRef` in `SemaContext::importedMemberTypes_` — so the
// classification is the OTHER module's, read through `recordMemberType`.
//
// ⚠ THE ROWS BELOW ARE NOT ALL LEAK ORACLES. The retained string in an
// `Object`-typed field is the documented #8214 residual leak (nothing may
// release it soundly), so a row that stores a HEAP string runs with
// LeakSanitizer OFF and the generated-code AddressSanitizer ON — the
// two-module twin of `runAotCheckedLenient`. The rows that store no heap
// string (the literal) keep the leak oracle on.
//
// ⚠ NO SKIP GUARD ABSORBS A TOOLCHAIN FAILURE. Every row asserts the run
// itself; a compile/link failure leaves `r.ran` false and the row RED.

#include <cstdlib>
#include <string>
#include <vector>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;

namespace {

// The library module of the issue's repro: a public `Object`-typed field, a
// class instance to store (the field's OTHER occupant — an `Object` slot may
// hold either), and the runtime string producer.
const char* kLib =
    "module Lib; "
    "public class Foo "
    "  public V: int := 7; "
    "end Foo; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "  public method Put(f: Foo) "
    "    this.O := f; "
    "  end Put; "
    "end Holder; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; ";

// The importer: the runtime-built string goes into the IMPORTED class's
// `Object` field from a `do … end` block, and the field is read AFTER the
// block ended — the read is the use-after-free.
const char* kApp =
    "module App; "
    "import Lib; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var total := 0; "
    "    var keep := new Foo(); "
    "    while i < 200 do "
    "      var h := new Holder(); "
    "      do "
    "        var x := Make.Str(i); "
    "        h.O := x as Object; "
    "      end; "
    "      var junk := Make.Str(99999); "
    "      total := total + h.O.ToString().Length + junk.Length - junk.Length; "
    "      h.Put(keep); "
    "      total := total + (h.O as Foo)!.V; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// The SINGLE-MODULE control: the same shape, one module, no import. Prints
// `690` (the sum of the lengths of `Make.Str(0..199)`), and leaks the
// documented #8214 amount.
const char* kSingle =
    "module App; "
    "public class Holder "
    "  public O: Object := \"\"; "
    "end Holder; "
    "public class Make "
    "  public static method Str(n: int): string => \"s\" + n.ToString(); "
    "end Make; "
    "public class Program "
    "  public static method Main(): int "
    "    var i := 0; var total := 0; "
    "    while i < 200 do "
    "      var h := new Holder(); "
    "      do "
    "        var x := Make.Str(i); "
    "        h.O := x as Object; "
    "      end; "
    "      var junk := Make.Str(99999); "
    "      total := total + h.O.ToString().Length + junk.Length - junk.Length; "
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(total); "
    "    return 0; "
    "  end Main; "
    "end Program; ";

// The two-module ASan oracle with LeakSanitizer OFF — the two-module twin of
// `runAotCheckedLenient`. `runAotModulesChecked` arms LeakSanitizer, and the
// programs here carry the DOCUMENTED #8214 leak (the retained string in an
// `Object`-typed field), so a leak-armed oracle would fail every one of them
// for a leak that is not this issue's subject. The generated-code
// AddressSanitizer (use-after-free / double free / over-release) stays ON,
// which IS this issue's subject.
//
// The `asanOptionsOverride` string is the one `aot_support.hpp` documents for
// exactly this case ("If a future two-module row genuinely needs
// `detect_leaks=0`, it is three lines: it is the same `runAotModulesImpl`
// call with `detect_leaks=0:exitcode=99:abort_on_error=0`"). Pinning it here
// also makes the row INDEPENDENT of the suite-wide `BEZEL_AOT_DETECT_LEAKS`
// knob: this row never claims anything about leaks.
//
// The toolchain probe is the same one `runAotModulesChecked` uses, and a
// missing C toolchain is reported as a SKIP diagnostic (never absorbed), so a
// box that cannot link ASan fails the row loudly instead of passing
// vacuously.
JitResult runAotModulesNoLeak(const std::vector<ModuleSource>& modules,
                               const char* entry) {
    static const bool toolchainProbeOk = [] {
        return std::system("clang --version > /dev/null 2>&1") == 0;
    }();
    if (!toolchainProbeOk) {
        JitResult r;
        r.diagnostics.report(
            bezel::Severity::Error, bezel::SourceLocation{},
            "runAotModulesNoLeak: SKIP — C toolchain (clang) unavailable for "
            "the ASan link");
        return r;
    }
    bezel::codegen::CodegenOptions opts;
    opts.checkedContracts = true;  // §22.16 checked-CONTRACTS mode
    opts.sanitizeAddress = true;   // generated-code AddressSanitizer
    return bezel::test::runAotModulesImpl(
        modules, entry, "runAotModulesNoLeak", std::move(opts),
        "detect_leaks=0:exitcode=99:abort_on_error=0");
}

// The library + importer pair of the issue's repro.
std::vector<ModuleSource> reproModules() {
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", kLib});
    mods.push_back({"App", kApp});
    return mods;
}

}  // namespace

// ===========================================================================
// THE ISSUE. A runtime-built string stored into an IMPORTED class's public
// `Object`-typed field must be retained by the store (§28.9: a store into a
// runtime-managed slot takes its own reference), because the field outlives
// the store's scope.
//
// PRE-FIX (base `74c9d282`): the program aborts — `free(): invalid pointer`,
// rc 134; under the generated-code ASan a heap-use-after-free READ in `main`.
// The harness sees the sanitizer report, so `r.ran` is false and this row is
// RED.
//
// POST-FIX: the program prints `2090` (the sum of the lengths of
// `Make.Str(0..199)` = 690, plus 200 × the stored `Foo`'s `V` = 7) and the
// ASan report is gone. The retained string in the `Object` field is the
// DOCUMENTED #8214 residual leak, so this row runs with LeakSanitizer OFF —
// it claims "no use-after-free", not "no leak".
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           imported_object_field_string_store_retains_the_string) {
    JitResult r = runAotModulesNoLeak(reproModules(), "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "2090\n");
}

// ===========================================================================
// THE SINGLE-MODULE CONTROL. The same shape in ONE module — the store is
// into a class compiled HERE, so the #8214 retain applies and the program is
// clean. It must keep working: it prints `690` and has no memory error.
//
// Its #8214 leak is the documented residual (the field's +1 is never
// released), so this row also runs with LeakSanitizer OFF; the LEAK is
// observed by the oracle-live row below, which is where the leak oracle
// lives.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           same_module_object_field_string_store_control_is_clean) {
    std::vector<ModuleSource> mods;
    mods.push_back({"App", kSingle});
    JitResult r = runAotModulesNoLeak(mods, "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "690\n");
}

// ===========================================================================
// THE ORACLE-LIVE POSITIVE CONTROL. The same single-module control under the
// LEAK-ARMED oracle (`runAotChecked`, `detect_leaks=1`): the program still
// prints `690`, and the sanitizer REPORTS the documented #8214 leak.
//
// This is what proves the sanitizer is attached to the generated code: a
// program with a KNOWN memory defect is REPORTED. Without it, a green
// "no use-after-free" row on the cross-module program above would be
// indistinguishable from "no sanitizer was ever linked".
//
// The leak is the DOCUMENTED #8214 residual — the retained string in an
// `Object`-typed field, which nothing may release soundly until a runtime
// discriminator exists (#8215 / #5059) — so this row PINS it: the report must
// name LeakSanitizer, and it must NOT be a use-after-free.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           oracle_is_live_single_module_control_still_reports_the_8214_leak) {
    JitResult r = bezel::test::runAotChecked(kSingle);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    // The program RAN and printed its value: the leak is at exit, after the
    // program's own output.
    BEZEL_EXPECT_EQ(r.stdoutText, "690\n");
    // The sanitizer fired on the documented leak (not on a use-after-free).
    bool sawLeak = false;
    bool sawUseAfterFree = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("LeakSanitizer") != std::string::npos)
            sawLeak = true;
        if (d.message.find("use-after-free") != std::string::npos)
            sawUseAfterFree = true;
    }
    BEZEL_EXPECT_TRUE(sawLeak);
    BEZEL_EXPECT_FALSE(sawUseAfterFree);
}

// ===========================================================================
// CONTROL — A STRING LITERAL. A literal is a SENTINEL reference (the
// runtime's literal rc value): `bz_string_retain` / `bz_string_release` are
// no-ops on it, so the store's retain must not change it and the row stays
// LEAK-CHECKED (a literal store allocates nothing).
//
// This is the control the issue's matrix names: a literal is not a
// runtime-built string, so it needs no retain — the fix must not change it
// and must not leak it.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           imported_object_field_string_literal_store_is_clean) {
    const char* lib =
        "module Lib; "
        "public class Holder "
        "  public O: Object := \"\"; "
        "end Holder; ";
    const char* app =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    h.O := \"lit\"; "
        "    Console.WriteLine(h.O.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", lib});
    mods.push_back({"App", app});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "lit\n");
}

// ===========================================================================
// CONTROL — A CLASS INSTANCE. A class reference stored into an `Object`-typed
// field is NOT a string and NOT a box: it is the borrow case (§9.7 — an
// `Object` position announces no source). The field must neither retain it (a
// `bz_string_retain` on a class instance would corrupt it) nor release it.
//
// The instance is passed as a PARAMETER, because §28.3's borrowed-field store
// rule rejects a scope-bound local as the source; the caller binds it with
// `using var` so its own lifetime is discharged exactly once.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           imported_object_field_class_instance_store_is_clean) {
    const char* lib =
        "module Lib; "
        "public class Foo "
        "  public V: int := 7; "
        "end Foo; "
        "public class Holder "
        "  public O: Object := \"\"; "
        "  public method Put(f: Foo) "
        "    this.O := f; "
        "  end Put; "
        "end Holder; ";
    const char* app =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    using var t := new Foo(); "
        "    h.Put(t); "
        "    Console.WriteLine(h.O.ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", lib});
    mods.push_back({"App", app});
    JitResult r = bezel::test::runAotModulesChecked(std::move(mods), "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Lib.Foo\n");
}

// ===========================================================================
// CONTROL — A PUBLIC `string`-TYPED FIELD (the neighbouring site). A `string`
// field has its OWN reference discipline (retain-new / release-old), which is
// symbol-based and therefore already cross-module correct. The row pins that
// the #8306 fix did not disturb it.
//
// The stored value is a runtime-built string, and the holder is never
// disposed, so the field's own +1 is not released either — the string
// field's discipline is not this issue's subject, and the row runs with
// LeakSanitizer OFF. It claims "no use-after-free", not "no leak".
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           imported_string_field_string_store_is_clean) {
    const char* lib =
        "module Lib; "
        "public class Holder "
        "  public S: string := \"\"; "
        "end Holder; "
        "public class Make "
        "  public static method Str(n: int): string => \"s\" + n.ToString(); "
        "end Make; ";
    const char* app =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    do "
        "      var x := Make.Str(7); "
        "      h.S := x; "
        "    end; "
        "    Console.WriteLine(h.S); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", lib});
    mods.push_back({"App", app});
    JitResult r = runAotModulesNoLeak(mods, "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s7\n");
}

// ===========================================================================
// CONTROL — A PRIVATE `Object` FIELD. §13.1: `private` is visible to the
// declaring class only, so the dependency does not export it (only PUBLIC and
// PROTECTED members reach the metadata) and the importer cannot reach it at
// all. The store therefore happens INSIDE the library — in the declaring
// class's own method — where the #8214 retain already applies.
//
// This row pins that the fix does not change the private field's behaviour:
// the store is the library's own compilation, and it is retained there.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           imported_private_object_field_store_is_retained_in_the_library) {
    const char* lib =
        "module Lib; "
        "public class Holder "
        "  private O: Object := \"\"; "
        "  public method PutS(s: string) "
        "    this.O := s; "
        "  end PutS; "
        "  public method Get(): Object => this.O; "
        "end Holder; "
        "public class Make "
        "  public static method Str(n: int): string => \"s\" + n.ToString(); "
        "end Make; ";
    const char* app =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var h := new Holder(); "
        "    do "
        "      var x := Make.Str(7); "
        "      h.PutS(x); "
        "    end; "
        "    Console.WriteLine(h.Get().ToString()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", lib});
    mods.push_back({"App", app});
    JitResult r = runAotModulesNoLeak(mods, "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "s7\n");
}

// ===========================================================================
// CONTROL — A MUTABLE `Object`-TYPED STATIC FIELD. There is none in the
// language: §22.12 rule 2 admits only the primitive/enum value statics, the
// plain heap-free inline-array fields and the synchronized-handle/task-cache
// reference forms, so `public static O: Object := "";` is a COMPILE ERROR.
//
// The row pins that the front end still rejects it — in the DEPENDENCY, so
// the two-module build fails there — so the "static field" site of the
// defect list is closed by the front end rather than by a missing retain.
// The diagnostic is the front end's own; the row asserts the compile fails
// and names the rule.
// ===========================================================================
BEZEL_TEST(object_string_store_retain_imported_8306,
           mutable_object_static_field_is_rejected_by_the_front_end) {
    const char* lib =
        "module Lib; "
        "public class Holder "
        "  public static O: Object := \"\"; "
        "end Holder; ";
    const char* app =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    std::vector<ModuleSource> mods;
    mods.push_back({"Lib", lib});
    mods.push_back({"App", app});
    JitResult r = bezel::test::runAotModules(mods, "App");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_FALSE(r.ran);
    bool mentions = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("22.12") != std::string::npos) mentions = true;
    BEZEL_EXPECT_TRUE(mentions);
}
