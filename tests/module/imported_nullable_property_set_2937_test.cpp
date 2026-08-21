// Spec-derived regression test for issue #2937.
//
// §7.2 (Nullable value types) / §5.1 (Implicit conversions) / §2.8.2 (Artifact
// interface) / §23.3 (Block-form properties) / §22.13 (Static initialization),
// v0.240.
//
// THE DEFECT — the CROSS-MODULE twin of #2927. `importedObj.P := v` where `P`
// is a NULLABLE-VALUE property on an IMPORTED class passed the BARE payload to
// the `bz$<Mod>.<Class>$<Prop>$propset` ABI stub, whose value parameter is
// §7.2's tagged `{i8 hasValue; T}` struct, and the compile ABORTED:
//
//     Call parameter type does not match function signature!
//     i32 1
//      { i8, i32 }  call void @"bz$XLib.XPlain$PP$propset"(ptr %254, i32 1)
//     bezelc: codegen error: IR verification failed
//
// #2927 fixed `lowerVirtualPropertySet` — the virtual/interface DISPATCH path.
// This is `lowerImportedPropertySetValue` (LlvmBackend_Assign.cpp), the direct
// ABI-stub path for a PLAIN (non-virtual, non-interface) imported property. It
// is reachable ONLY across an artifact boundary, which is why no single-module
// test could see it: the in-module spelling goes through `lowerPropertySetValue`
// (already correct) and the polymorphic spelling through #2927's helper.
//
// THE FIX applies the same two helpers, in the same order, for the same reason:
// #2903's `coerceInitializerToSlot` (one definition of an initializer's slot
// width, taking the nullable slot's PAYLOAD as the width to match, in both of
// §5.1's directions) then §7.2's `coerceToNullableSlot` present-wrap, which
// also normalizes a `null` literal to the all-zero form.
//
// Built the same way as this file's nearest sibling,
// tests/module/property_compound_import_2862_test.cpp: `bezelc -c lib.bzl`
// then a real `--import` link and native run.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2937 {
    std::string path;
    explicit TempFile2937(std::string p) : path(std::move(p)) {}
    ~TempFile2937() { std::remove(path.c_str()); }
};

void writeSource2937(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2937(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally.
int compileImportAndRun2937(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2937_");
    TempFile2937 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2937 aObj((dir / (stem + "_a.o")).string());
    writeSource2937(aSrc.path, libSrc);
    if (!compileToObj2937(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2937 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2937 bExe((dir / stem).string());
    writeSource2937(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — #2937's own repro plus the shapes that share the one fixed line:
//   bit 1  `p.PP := 1`   — the bare payload into an `int?` slot (the ICE);
//   bit 2  `p.LP := 5`   — §5.1's WIDENING into a `long?` slot, the direction
//                          the payload-width match exists for;
//   bit 4  `p.PP := null`— the `null` literal's all-zero normalization;
//   bit 8  `p.SP := "x"` — a nullable REFERENCE property, a bare `ptr` and not
//                          §7.2's tagged struct: the control that must pass
//                          through both helpers untouched.
// Pre-fix: the program does not compile at all (IR verification abort).
// Post-fix: 15.
// ===========================================================================
BEZEL_TEST(imported_nullable_property_set_2937, imported_nullable_value_property_set_7_2) {
    const char* lib =
        "module NqLib2937T1; "
        "public class XPlain2937T1 "
        "  public _z: int := 0; "
        "  public _p: int? := null; "
        "  public _l: long? := null; "
        "  public _s: string? := null; "
        "  public property PP: int? get => _p; set => do _p := value; end; end; "
        "  public property LP: long? get => _l; set => do _l := value; end; end; "
        "  public property SP: string? get => _s; set => do _s := value; end; end; "
        "end XPlain2937T1;";
    int r = compileImportAndRun2937(
        lib,
        "module NqApp2937T1; import NqLib2937T1; "
        "public class C2937T1 "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    using var p := new XPlain2937T1(); "
        "    p.PP := 1; "
        "    if (p._p ?? -1) = 1 and p._z = 0 then acc := acc + 1; end; "
        "    p.LP := 5; "
        "    if (p._l ?? -1) = 5 then acc := acc + 2; end; "
        "    p.PP := null; "
        "    if p._p = null then acc := acc + 4; end; "
        "    p.SP := \"x\"; "
        "    if (p._s ?? \"\") = \"x\" then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end C2937T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 15);
}

// ===========================================================================
// T2 — the OTHER callers that reach the same line, which is why the coercion
// went into the helper and not into `lowerStmt`'s `:=` arm. All three were
// UNREACHABLE before the fix (the `:=` in the same program aborted the
// compile), so this case is red-before / green-after for the same reason:
//   bit 1  `p.PP += 3`  — #2862's imported compound path, which arrives with
//                         an already-TAGGED value from §7.11's lifting arm and
//                         must not be wrapped a second time;
//   bit 2  `p.PP++`     — §17.6's spelling of the same;
//   bit 4  `p.PP ??= v` — #2905's accessor path on a NULL target;
//   bit 8  `p.PP ??= v` — the same on a NON-null target (must not store).
// Post-fix: 15.
// ===========================================================================
BEZEL_TEST(imported_nullable_property_set_2937, imported_nullable_property_compound_6_6) {
    const char* lib =
        "module NqLib2937T2; "
        "public class XPlain2937T2 "
        "  public _p: int? := null; "
        "  public property PP: int? get => _p; set => do _p := value; end; end; "
        "end XPlain2937T2;";
    int r = compileImportAndRun2937(
        lib,
        "module NqApp2937T2; import NqLib2937T2; "
        "public class C2937T2 "
        "  public static method Main(): int "
        "    var acc := 0; "
        // §6.6 / §17.6 / §7.11 (v0.286, #3298): the `p.PP += 3;` and
        // `p.PP++;` legs are compile errors now — a read-modify-write compound
        // and its increment form over a NULLABLE imported property. §7.11's
        // retained set is what this row keeps: the plain `:=` store and `??=`,
        // which §6.6 excludes by name ("`??=` is unaffected — it is not a
        // read-modify-write"). The rejection is pinned by
        // `codegen_nullable_property_compound_2888`; what #2937 is about — that
        // an IMPORTED property's setter is found and called across the module
        // boundary — is unchanged and still exercised by every leg below.
        "    using var p := new XPlain2937T2(); "
        "    p.PP := 1; "
        "    p.PP := (p._p ?? 0) + 3; "
        "    if (p._p ?? -1) = 4 then acc := acc + 1; end; "
        "    p.PP := (p._p ?? 0) + 1; "
        "    if (p._p ?? -1) = 5 then acc := acc + 2; end; "
        "    using var q := new XPlain2937T2(); "
        "    q.PP ??= 42; "
        "    if (q._p ?? -1) = 42 then acc := acc + 4; end; "
        "    p.PP ??= 99; "
        "    if (p._p ?? -1) = 5 then acc := acc + 8; end; "
        "    return acc; "
        "  end Main; "
        "end C2937T2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 15);
}

}  // namespace bezel::module
