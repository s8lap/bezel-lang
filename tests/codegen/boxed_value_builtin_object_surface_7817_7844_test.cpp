// Codegen — §8 / §8.5 / §8.6 / §8.3 / stdlib §2.5 / §2.6 / #7817 / #7844:
// the Object surface of a BOXED built-in VALUE type speaks about the VALUE.
//
// THE DEFECTS (both MEASURED on this fix's parent, `c4600284`).
//
//   #7817 — `var o: Object := ts; o.ToString()` printed "System.TimeSpan"
//     (and "System.Guid" / "System.DateTime"). The value-builtin box vtable's
//     slot 3 is `valueBuiltinBoxToStringThunk`, which returns the type's
//     QUALIFIED NAME — §8.5's "the type's qualified name" default, which is
//     the answer for a CLASS with no ToString. stdlib §2.5 / §2.6 give these
//     value types their OWN ToString (the ISO-8601 / canonical / "c"
//     rendering), and the UNBOXED spelling (`ts.ToString()`) already rendered
//     the value: two spellings of one value disagreed inside one program.
//     The issue's own second repro — an `extension Object` body's
//     `this.ToString()` — showed the same.
//
//   #7844 — `var o1: Object := DateTime.FromTicks(0L); var o2: Object :=
//     DateTime.FromTicks(0L); o1.Equals(o2)` answered FALSE. The value-builtin
//     box vtable's slot 4 is `boxDefaultEqualsThunk`, the §8.3 REFERENCE-
//     IDENTITY default, while stdlib §2.5 gives DateTime (and every other
//     §2.5 / §2.6 value type) VALUE equality. The PRIMITIVE box vtable already
//     answered by value (`primBoxEqualsThunk`, #961); the value-builtin box
//     was the outlier.
//
// THE FIX (src/codegen/LlvmBackend_Layout.cpp — `Emitter::boxValue`). For a
// built-in value type only, the box's vtable is REPLACED by one whose three
// Object slots read the box's PAYLOAD: slot 3 renders it through
// `emitValueToString` (the very renderer the UNBOXED value's own ToString
// uses — ONE authority, so the two spellings cannot drift), slot 4 compares
// the two payloads through `emitValueEquality` (the same helper `=` and a
// record field's equality use), slot 5 hashes the payload through
// `emitValueHashCode` (so §8.3's "a.Equals(b) ⇒ a.GetHashCode() =
// b.GetHashCode()" invariant holds for the boxed surface too). The
// replacement keeps the base vtable's descriptor / itmap / Dispose slots
// VERBATIM, so the box's type tag is unchanged.
//
// ORACLE DISCIPLINE. These rows assert the POSITIVE: the EXACT stdout and the
// exact exit code. A row that only asserted "the program ran" would be
// vacuous; a row that asserted the printed value WITHOUT pinning the expected
// text would not pin the fix. The two rendering rows (A and B) additionally
// run under the harness's ASan/LeakSanitizer leg (`runAotChecked`), so a
// dispatch that read freed memory would be caught by the sanitizer rather
// than by the text.
//
// ⚠ A DISCLOSED RESIDUAL, NOT CLOSED HERE. The §18.5 INTERPOLATION-HOLE
// spelling of the same box (`$"{o}"` on an `Object` local whose INITIALIZER
// is directly a value-builtin expression) still renders the type name. That
// is a DIFFERENT mechanism: the hole does not dispatch the vtable, it takes
// `tryOpaqueSlotBuiltinRender` (src/codegen/LlvmBackend_String.cpp), whose
// #2968 static narrowing classifies the slot's INITIALIZER type — a boxed
// value-builtin — as a "vtable-less builtin class" and renders its qualified
// name. That file is not this lane's, and the narrowing is not the vtable
// this commit replaces. Every OTHER interpolation spelling already renders
// the value through the vtable this commit replaces (measured: a hole whose
// source is a call, `$"{Box(TimeSpan.FromHours(1.0))}"`, prints "01:00:00").
// The issue's own repros — `o.ToString()` and the `extension Object` body's
// `this.ToString()` — are closed by this commit.
//
// §8.5 (ToString), §8.6 (Boxing), §8 (the Object class — its public surface),
// §8.3 (value equality and the hash invariant), stdlib §2.5 (DateTime /
// TimeSpan / DateOnly / TimeOnly) and stdlib §2.6 (Guid).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// (A) THE ISSUE'S HEADLINE (#7817) — a boxed TimeSpan rendered through the
// Object surface prints its VALUE ("01:00:00"), not "System.TimeSpan".
const char* kTimeSpanBoxToString =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var ts := TimeSpan.FromHours(1.0); "
    "    var o: Object := ts; "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (B) THE GUID ROW (#7817) — the canonical 36-character form (stdlib §2.6).
const char* kGuidBoxToString =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var g := Guid.Empty; "
    "    var o: Object := g; "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (C) THE DateTime ROW (#7817) — the ISO-8601 rendering (stdlib §2.5).
const char* kDateTimeBoxToString =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var d := DateTime.FromTicks(0L); "
    "    var o: Object := d; "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (D) THE REST OF THE §2.5 FAMILY (#7817) — DateOnly ("yyyy-MM-dd") and
// TimeOnly ("HH:mm:ss") beside TimeSpan's constant "c" form.
const char* kFamilyBoxToString =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var t := TimeSpan.FromTicks(5L); "
    "    var a: Object := t; "
    "    var d := new DateOnly(2024, 3, 15); "
    "    var b: Object := d; "
    "    var u := new TimeOnly(1, 2, 3, 4); "
    "    var c: Object := u; "
    "    Console.WriteLine(a.ToString()); "
    "    Console.WriteLine(b.ToString()); "
    "    Console.WriteLine(c.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (E) THE ISSUE'S SECOND REPRO (#7817) — inside an `extension Object` body,
// `this.ToString()` is the boxed value's ToString: the extension's `this` is
// the OBJECT, so the call dispatches the very vtable this commit replaces.
const char* kExtensionObjectToString =
    "module M; "
    "extension Object "
    "  public method Show(): string => \"<\" + this.ToString() + \">\"; "
    "end; "
    "public class C "
    "  public static method Main(): int "
    "    var ts := TimeSpan.FromHours(1.0); "
    "    Console.WriteLine(ts.Show()); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (F) THE ISSUE'S HEADLINE (#7844) — two boxes of EQUAL DateTime values
// compare EQUAL, and two boxes of UNEQUAL values do not. The second line is
// the control: the fix must not make every pair of boxes equal.
const char* kDateTimeBoxEquals =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var o1: Object := DateTime.FromTicks(0L); "
    "    var o2: Object := DateTime.FromTicks(0L); "
    "    var o3: Object := DateTime.FromTicks(1L); "
    "    Console.WriteLine(o1.Equals(o2)); "
    "    Console.WriteLine(o1.Equals(o3)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (G) THE Guid ROW (#7844) — the same value equality for the §2.6 type.
const char* kGuidBoxEquals =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var o1: Object := Guid.Empty; "
    "    var o2: Object := Guid.Empty; "
    "    var o3: Object := Guid.NewGuid(); "
    "    Console.WriteLine(o1.Equals(o2)); "
    "    Console.WriteLine(o1.Equals(o3)); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (H) THE §8.3 HASH INVARIANT (#7844) — equal values hash equally, so the
// two boxes' `GetHashCode()` agree. The two hashes are compared through
// LOCALS (and with the language's `=` equality operator, not `==`), so the
// row observes the INVARIANT and not a hash constant.
const char* kBoxHashInvariant =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var o1: Object := DateTime.FromTicks(0L); "
    "    var o2: Object := DateTime.FromTicks(0L); "
    "    var h1: int := o1.GetHashCode(); "
    "    var h2: int := o2.GetHashCode(); "
    "    Console.WriteLine(h1 = h2); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (I) THE TYPE TAG IS UNCHANGED (#7817) — the replacement vtable keeps the
// base's descriptor, so the §12.7 type test (`is`) still walks the same
// chain: a boxed DateTime IS a DateTime and is NOT a Guid.
const char* kTypeTagUnchanged =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var o: Object := DateTime.FromTicks(0L); "
    "    Console.WriteLine(o is DateTime); "
    "    Console.WriteLine(o is Guid); "
    "    return 0; "
    "  end Main; "
    "end C;";

// (J) THE TWO SPELLINGS AGREE (#7817) — the boxed `ToString()` and the
// unboxed `ToString()` of ONE value print the SAME text. Before the fix they
// disagreed ("System.TimeSpan" vs "01:00:00") inside one program.
const char* kUnboxedAgreesWithBoxed =
    "module M; "
    "public class C "
    "  public static method Main(): int "
    "    var ts := TimeSpan.FromHours(1.0); "
    "    var o: Object := ts; "
    "    Console.WriteLine(ts.ToString()); "
    "    Console.WriteLine(o.ToString()); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) #7817's headline: the boxed TimeSpan renders its VALUE, under the
// ASan/LeakSanitizer leg too (a dispatch that read freed memory would be
// reported by the sanitizer).
// stdlib §2.5 `TimeSpan` / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_timespan_tostring_renders_the_value_stdlib_2_5) {
    JitResult r = runAotChecked(kTimeSpanBoxToString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:00:00\n");
}

// =====================================================================
// (B) #7817 for the §2.6 type: the canonical 36-character Guid form.
// stdlib §2.6 `Guid` / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_guid_tostring_renders_the_value_stdlib_2_6) {
    JitResult r = runAotChecked(kGuidBoxToString);
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00000000-0000-0000-0000-000000000000\n");
}

// =====================================================================
// (C) #7817 for the §2.5 DateTime: the ISO-8601 rendering.
// stdlib §2.5 `DateTime` / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_datetime_tostring_renders_the_value_stdlib_2_5) {
    JitResult r = runAot(kDateTimeBoxToString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0001-01-01T00:00:00\n");
}

// =====================================================================
// (D) #7817 for the rest of the §2.5 family: TimeSpan's "c" form,
// DateOnly's "yyyy-MM-dd" and TimeOnly's "HH:mm:ss".
// stdlib §2.5 `TimeSpan` / `DateOnly` / `TimeOnly` / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_family_tostring_renders_the_values_stdlib_2_5) {
    JitResult r = runAot(kFamilyBoxToString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00:00:00.0000005\n2024-03-15\n01:02:03\n");
}

// =====================================================================
// (E) #7817's SECOND repro: inside an `extension Object` body,
// `this.ToString()` is the boxed value's ToString.
// stdlib §2.5 / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           extension_object_this_tostring_renders_the_value_stdlib_2_5) {
    JitResult r = runAot(kExtensionObjectToString);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "<01:00:00>\n");
}

// =====================================================================
// (F) #7844's headline: two boxes of EQUAL DateTime values compare EQUAL;
// two boxes of UNEQUAL values do not.
// stdlib §2.5 `DateTime` / §8.3.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_datetime_equals_compares_the_value_stdlib_2_5) {
    JitResult r = runAot(kDateTimeBoxEquals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// =====================================================================
// (G) #7844 for the §2.6 type: the same value equality for Guid.
// stdlib §2.6 `Guid` / §8.3.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_guid_equals_compares_the_value_stdlib_2_6) {
    JitResult r = runAot(kGuidBoxEquals);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// =====================================================================
// (H) #7844's hash half: §8.3's "a.Equals(b) ⇒ a.GetHashCode() =
// b.GetHashCode()" invariant holds for the boxed surface.
// stdlib §2.5 / §8.3.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_equal_values_hash_equally_spec_8_3) {
    JitResult r = runAot(kBoxHashInvariant);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\n");
}

// =====================================================================
// (I) The type tag is UNCHANGED: the replacement vtable keeps the base's
// descriptor, so the §12.7 type test (`is`) still walks the same chain.
// stdlib §2.5 / §8.6 / §12.7.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_type_tag_still_walks_the_descriptor_chain_spec_19_3) {
    JitResult r = runAot(kTypeTagUnchanged);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\n");
}

// =====================================================================
// (J) The two spellings of ONE value agree: the unboxed `ToString()` and
// the boxed `Object.ToString()` print the SAME text.
// stdlib §2.5 / §8.5 / §8.6.
// =====================================================================
BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           boxed_and_unboxed_tostring_agree_stdlib_2_5) {
    JitResult r = runAot(kUnboxedAgreesWithBoxed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:00:00\n01:00:00\n");
}

// =====================================================================
// (K) THE CROSS-MODULE ROW — the box's TYPE TAG and its Object surface
// survive a module boundary.
//
// §2.8.3 rule 4 (program-wide type identity) / §8.6 (boxing) / §8.5 / §8.3.
//
// ⚠ THIS ROW EXISTS BECAUSE THE FIRST FORM OF THE FIX WAS A CROSS-MODULE
// REGRESSION. The replacement box vtable `pvtvb$<nm>` was minted LAZILY, in
// the module that happened to box. `bz_obj_is_string` decides "is this
// Object a string?" by asking whether the value's word 0 is listed in the
// CALLING module's `bz$vtable$table` — a per-module array built from THAT
// module's `allVtables_`. A lazily-minted vtable is listed in the boxing
// module's table only, so the CONSUMING module answered "this is a string":
// `o is DateTime` answered False, `o.ToString()` printed garbage bytes, and
// `o.GetHashCode()` hashed garbage. The fix mints the replacement EAGERLY,
// FOR EVERY MODULE (setupBoxDescriptors → valueBuiltinBoxVtableFor), so every
// module's table lists it.
//
// MEASURED (two modules, `bezelc -c -o libdt.o libdt.bzl` then
// `bezelc --import libdt.o -o maindt maindt.bzl`):
//
//   parent c4600284 : isDT=True  str=System.DateTime
//   the lazy fix    : isDT=False str=<garbage byte>
//   this fix        : isDT=True  str=0001-01-01T00:00:00
//
// (The parent's `str=` is the class default — the type's qualified name,
// §8.5 — which is exactly the answer #7817 is about; the fix replaces it
// with the VALUE's rendering, and keeps the parent's correct TYPE TAG.)
// =====================================================================

namespace {

// The library module: it boxes a DateTime and a TimeSpan and hands the boxes
// back as `Object` — the cross-module shape the reviewer measured.
const char* kXmodLib =
    "module WTLib7817; "
    "public static class Holder "
    "  public static method MakeDT(): Object "
    "    var d := DateTime.FromTicks(0L); "
    "    var o: Object := d; "
    "    return o; "
    "  end MakeDT; "
    "  public static method MakeTS(): Object "
    "    var t := TimeSpan.FromHours(1.0); "
    "    var o: Object := t; "
    "    return o; "
    "  end MakeTS; "
    "end Holder;";

// The application module: it CONSUMES the boxes.
const char* kXmodApp =
    "module WTApp7817; "
    "import WTLib7817; "
    "public class Program "
    "  public static method Main(): int "
    "    var o: Object := Holder.MakeDT(); "
    "    Console.WriteLine(\"isDT=\" + (o is DateTime).ToString()); "
    "    Console.WriteLine(\"str=\" + o.ToString()); "
    "    var t: Object := Holder.MakeTS(); "
    "    Console.WriteLine(\"isTS=\" + (t is TimeSpan).ToString()); "
    "    Console.WriteLine(\"tsStr=\" + t.ToString()); "
    "    var local: Object := DateTime.FromTicks(0L); "
    "    var h1: int := o.GetHashCode(); "
    "    var h2: int := local.GetHashCode(); "
    "    Console.WriteLine(\"hashEq=\" + (h1 = h2).ToString()); "
    "    return 0; "
    "  end Main; "
    "end Program;";

struct XmodTempFile {
    std::string path;
    explicit XmodTempFile(std::string p) : path(std::move(p)) {}
    ~XmodTempFile() { std::remove(path.c_str()); }
};

void xmodWriteSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct XmodRunResult { int code; std::string out; };

XmodRunResult xmodRunCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

std::string xmodTrim(std::string s) {
    while (!s.empty() &&
           (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

}  // namespace

BEZEL_TEST(codegen_boxed_value_builtin_object_surface_7817_7844,
           cross_module_box_keeps_its_type_tag_and_renders_the_value_spec_2_8_3) {
    auto dir = wtScratchDir("wt-7817-xmod");
    std::string stem = unique_stem("bezel_7817xmod_");
    auto libSrc = std::make_shared<XmodTempFile>((dir / (stem + "_lib.bzl")).string());
    auto libObj = std::make_shared<XmodTempFile>((dir / (stem + "_lib.o")).string());
    auto appSrc = std::make_shared<XmodTempFile>((dir / (stem + "_app.bzl")).string());
    auto appExe = std::make_shared<XmodTempFile>((dir / (stem + "_app")).string());
    xmodWriteSource(libSrc->path, kXmodLib);
    xmodWriteSource(appSrc->path, kXmodApp);

    // Module 1 — the library, compiled to an OBJECT (no link).
    bezel::Driver libDriver;
    bezel::CompileResult libR =
        libDriver.compileObject(libSrc->path, libObj->path);
    BEZEL_EXPECT_TRUE(libR.success);
    if (!libR.success) return;

    // Module 2 — the application, compiled against the library's object.
    bezel::Driver appDriver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj->path;
    d.objectPath = libObj->path;
    deps.push_back(d);
    bezel::CompileResult appR =
        appDriver.compileFile(appSrc->path, appExe->path, deps);
    BEZEL_EXPECT_TRUE(appR.success);
    if (!appR.success) return;

    auto [code, out] = xmodRunCapture(appExe->path);
    BEZEL_EXPECT_EQ(code, 0);
    // The load-bearing pin: the EXACT stdout. Every column is load-bearing:
    // `isDT`/`isTS` are the TYPE TAG across the boundary (§2.8.3 rule 4), the
    // two `str=` lines are the VALUE rendering (#7817), and `hashEq` is §8.3's
    // hash invariant across the boundary.
    BEZEL_EXPECT_EQ(xmodTrim(out),
                    std::string("isDT=True\n"
                                "str=0001-01-01T00:00:00\n"
                                "isTS=True\n"
                                "tsStr=01:00:00\n"
                                "hashEq=True"));
}
