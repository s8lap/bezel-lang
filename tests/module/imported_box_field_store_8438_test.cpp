// Module — #8438 (§28.9 / §8.6 / §2.4, v0.357): a store into an IMPORTED
// class's box-storage `Object` field did not follow the field's discipline.
//
// THE RULE. §28.9, *Boxed value types*: "a boxed value is an immutable shared
// reference" -- every holder of a box holds a reference of its own. A
// box-storage field is one such holder: every store gives the field a
// reference and releases the one it displaces, and the holder's disposer
// releases the last one (#8344, #8215).
//
// THE DEFECT. That classification was computed per MODULE
// (`Emitter::collectBoxStorageFields` reads the classes of the module being
// compiled). A module that IMPORTS the class and writes the public field
// directly knew nothing about it and stored into an uncounted slot:
//
//   * `b.o := a.o` took no reference, and the library's disposer released
//     one for each holder -- "bezel: fatal: refcount over-release (box)", or
//     a read of the freed box (the issue's measurement);
//   * a store that displaced a box (`a.o := i + 1`, `a.o := null`) never
//     released it -- a leak;
//   * and for a box-ONLY library field, an importer's store of a class
//     instance made the library's disposer release that instance as a box.
//
// THE FIX. The library exports, for each dynamic box-storage field an importer
// can write, a store thunk `bz$<Class>$<field>$boxfieldset`; the importer
// references it weakly and calls it (`Emitter::lowerImportedBoxFieldStore`).
// A field an importer can write is not box-ONLY in a library build -- it
// takes the dynamic (run-time discriminated) discipline instead, wherever
// that discipline is available for it.
//
// ORACLE. `runAotModulesChecked` (generated-code ASan + LeakSanitizer) on a
// real two-module build; every program loops its shape 100 or 200 times and
// reads the value back, because the freed box is read only inside
// uninstrumented runtime helpers (ASan is silent on it) -- the over-release
// abort, the leak report or the wrong sum is what fails.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

namespace {

// The issue's library (P4Lib): `o` is a DYNAMIC box-storage field.
std::string libDynamic(const std::string& name) {
    return "module " + name + "; "
           "public class Leaf "
           "  public v: int; "
           "  public constructor(v: int) this.v := v; end constructor; "
           "end Leaf; "
           "public class Holder "
           "  public o: Object?; "
           "  public constructor() end constructor; "
           "  public method Take(n: int) o := n; end Take; "
           "  public method Put(x: Object) o := x; end Put; "
           "  public method Read(): int "
           "    return (o as int) ?? ((o as Leaf)?.v ?? -1); "
           "  end Read; "
           "end Holder;";
}

// The control library of the issue's comment (P4LibB): every write the
// library makes is a box, so `o` was box-ONLY.
std::string libBoxOnly(const std::string& name) {
    return "module " + name + "; "
           "public class Leaf "
           "  public v: int; "
           "  public constructor(v: int) this.v := v; end constructor; "
           "end Leaf; "
           "public class Holder "
           "  public o: Object?; "
           "  public constructor() end constructor; "
           "  public method Take(n: int) o := n; end Take; "
           "  public method Read(): int "
           "    return (o as int) ?? ((o as Leaf)?.v ?? -1); "
           "  end Read; "
           "end Holder;";
}

std::string app(const std::string& name, const std::string& lib, int n,
                const std::string& body) {
    return "module " + name + "; import " + lib + "; "
           "public class Program "
           "  static method Fill(a: Holder, leaf: Leaf) a.o := leaf; end Fill; "
           "  static method Put(b: Holder, x: Object?) b.o := x; end Put; "
           "  static method Use(x: Object?, h: Holder): int "
           "    h.o := 5000; "
           "    return (x as int) ?? -1; "
           "  end Use; "
           "  public static method Main(): int "
           "    var sum := 0; "
           "    for var i in 0..<" + std::to_string(n) + " do "
           + body +
           "    end for; "
           "    Console.WriteLine(sum); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

JitResult run(const std::string& tag, bool dynamicLib, int n,
              const std::string& body) {
    const std::string lib = "L8438" + tag;
    const std::string appName = "A8438" + tag;
    return bezel::test::runAotModulesChecked(
        {{lib, dynamicLib ? libDynamic(lib) : libBoxOnly(lib)},
         {appName, app(appName, lib, n, body)}},
        appName);
}

void expectClean(const JitResult& r, const char* expected) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// The issue's App (p4_app_copy): two holders, the importer copies the field.
const char* kCopy =
    "      using var a := new Holder(); "
    "      using var b := new Holder(); "
    "      a.Take(i); "
    "      b.o := a.o; "
    "      sum += b.Read(); ";

// The copy, then the source holder displaces its box: `b` must still own one.
const char* kCopyThenSourceOverwritten =
    "      using var a := new Holder(); "
    "      using var b := new Holder(); "
    "      a.Take(i); "
    "      b.o := a.o; "
    "      a.Take(5000); "
    "      sum += b.Read(); ";

// The importer displaces a box with a box, twice, then clears the field.
const char* kOverwriteAndClear =
    "      using var a := new Holder(); "
    "      a.o := i + 7; "
    "      a.o := i; "
    "      sum += a.Read(); "
    "      a.o := null; "
    "      sum += a.Read() + 1; ";

// The importer displaces the LIBRARY's box, and the library the importer's.
const char* kMixedWriters =
    "      using var a := new Holder(); "
    "      a.Take(5000); "
    "      a.o := i; "
    "      sum += a.Read(); "
    "      a.Take(i); "
    "      sum += a.Read() - i; ";

// An Object-typed LOCAL minted with a box is stored, twice. The store makes
// the local escape the box-release pre-pass, so the reference the box was
// minted with is the one the field ends up owning (a CONTROL: this shape was
// balanced before the fix and must stay balanced).
const char* kLocalSource =
    "      using var a := new Holder(); "
    "      var x: Object? := i; "
    "      a.o := x; "
    "      a.o := x; "
    "      sum += a.Read(); ";

// A local that ALIASES another holder's box (a #8444 pin: it holds a
// reference of its own) is stored: the field takes its own reference.
const char* kPinnedLocalSource =
    "      using var a := new Holder(); "
    "      using var b := new Holder(); "
    "      a.Take(i); "
    "      var t: Object? := a.o; "
    "      b.o := t; "
    "      a.Take(5000); "
    "      sum += b.Read(); ";

// A PARAMETER (a borrow for the call) is stored: the field takes its own.
const char* kParameterSource =
    "      using var a := new Holder(); "
    "      using var b := new Holder(); "
    "      a.Take(i); "
    "      Program.Put(b, a.o); "
    "      a.Take(5000); "
    "      sum += b.Read(); ";

// THE MIRROR: nothing is displaced and nothing is copied -- one store, one
// read, one disposal.
const char* kMirror =
    "      using var a := new Holder(); "
    "      a.o := i; "
    "      sum += a.Read(); ";

// A class instance stored by the importer (a parameter-sourced borrow, §28.3)
// into a field the library only ever boxes into: the library's disposer must
// not release the instance as a box, and the displaced box is released.
const char* kClassInstanceIntoBoxField =
    "      using var leaf := new Leaf(i); "
    "      using var a := new Holder(); "
    "      a.Take(5000); "
    "      Program.Fill(a, leaf); "
    "      sum += a.Read(); ";

// THE READ TWINS (#8444 across the module boundary): the importer aliases the
// imported field, then the field's box is displaced -- by the importer's own
// store, by its clear, and inside a callee the alias was passed to.
const char* kAliasThenImporterOverwrite =
    "      using var a := new Holder(); "
    "      a.Take(i); "
    "      var t: Object? := a.o; "
    "      a.o := 5000; "
    "      sum += (t as int) ?? -1; ";

const char* kAliasThenImporterClear =
    "      using var a := new Holder(); "
    "      a.Take(i); "
    "      var t: Object? := a.o; "
    "      a.o := null; "
    "      sum += (t as int) ?? -1; ";

const char* kArgumentThenImporterOverwrite =
    "      using var a := new Holder(); "
    "      a.Take(i); "
    "      sum += Program.Use(a.o, a); ";

}  // namespace

// The issue's program as measured (#8438's comment). ORACLE-BLIND WITHOUT THE
// FIX: on a DYNAMIC field the second disposer's guarded release only READS the
// freed box, inside the uninstrumented runtime, so this row is green on both
// arms (valgrind: invalid reads). It stays as the literal repro; the two rows
// after it are the same defect in the forms this oracle does see.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_issue_repro_importer_copies_dynamic_field) {
    expectClean(run("A", /*dynamicLib=*/true, 100, kCopy), "4950\n");
}

// FAIL-WITHOUT: the box-only disposer releases unguarded -- "bezel: fatal:
// refcount over-release (box)".
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_issue_control_importer_copies_box_only_field) {
    expectClean(run("B", /*dynamicLib=*/false, 100, kCopy), "4950\n");
}

// FAIL-WITHOUT: the copy reads a freed box (wrong sum) and the disposer
// over-releases.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_copy_survives_source_overwrite_dynamic) {
    expectClean(run("C", true, 200, kCopyThenSourceOverwritten), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_copy_survives_source_overwrite_box_only) {
    expectClean(run("D", false, 200, kCopyThenSourceOverwritten), "19900\n");
}

// FAIL-WITHOUT: the displaced boxes leak (LeakSanitizer).
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_overwrite_and_clear_release_displaced_dynamic) {
    expectClean(run("E", true, 200, kOverwriteAndClear), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_overwrite_and_clear_release_displaced_box_only) {
    expectClean(run("F", false, 200, kOverwriteAndClear), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_and_library_displace_each_other) {
    expectClean(run("G", true, 200, kMixedWriters), "19900\n");
}

// CONTROL (green on the base too).
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_escaping_object_local_source_stays_balanced) {
    expectClean(run("H", true, 200, kLocalSource), "19900\n");
}

// FAIL-WITHOUT on the box-only library: over-release (two holders and the
// pin, two references). The dynamic-library twin is oracle-blind without the
// fix, like the issue's own row; it runs as the tip-side check.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_pinned_local_source_is_retained_by_the_field_box_only) {
    expectClean(run("P", false, 200, kPinnedLocalSource), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_pinned_local_source_is_retained_by_the_field_dynamic) {
    expectClean(run("R", true, 200, kPinnedLocalSource), "19900\n");
}

// FAIL-WITHOUT: over-release.

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_parameter_source_is_retained_by_the_field) {
    expectClean(run("Q", false, 200, kParameterSource), "19900\n");
}

// CONTROL (green on the base too): the never-displaced mirror.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_mirror_single_store_dynamic) {
    expectClean(run("I", true, 200, kMirror), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_mirror_single_store_box_only) {
    expectClean(run("J", false, 200, kMirror), "19900\n");
}

// FAIL-WITHOUT: the box-only library disposer releases the class instance as
// a box (its first field is decremented: the sum is wrong) and the displaced
// box leaks.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_class_instance_into_library_box_field_is_not_released) {
    expectClean(run("K", false, 200, kClassInstanceIntoBoxField), "19900\n");
}

// The read twins. FAIL-WITHOUT: the box the importer's store displaces is
// never released (LeakSanitizer); the alias itself is #8444's pin.
BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_alias_survives_importer_overwrite_dynamic) {
    expectClean(run("L", true, 200, kAliasThenImporterOverwrite), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_alias_survives_importer_overwrite_box_only) {
    expectClean(run("M", false, 200, kAliasThenImporterOverwrite), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_alias_survives_importer_clear) {
    expectClean(run("N", true, 200, kAliasThenImporterClear), "19900\n");
}

BEZEL_TEST(module_imported_box_field_store_8438,
           spec_28_9_importer_argument_survives_importer_overwrite) {
    expectClean(run("O", false, 200, kArgumentThenImporterOverwrite),
                "19900\n");
}
