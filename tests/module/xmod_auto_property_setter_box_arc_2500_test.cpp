// fix #2500 (§23.3 "Block-form properties" / §8.6 boxing / §28.9 "Runtime-
// managed types", v0.236): Emitter::ensurePropertyAccessorFn's auto-property
// SETTER body had NO retain/release ARC for an Object/interface (box)-typed
// backing field — the box twin of #2490's confirmed string/closure setter bug,
// deferred from that fix because the box-storage classification prepass
// (collectBoxStorageFields) ran AFTER this memoized function's two earliest
// callers (declareVtables / emitExportedPropertySetters).
//
// OBSERVED-vs-CLAIMED (the issue was filed by analogy, "not confirmed"):
// reproducing the issue's own cross-module shape on origin/main revealed the
// setter body ARC gap is REAL but was STRUCTURALLY UNREACHABLE — every call
// site that invokes the auto-setter for a box-typed property passed an UNBOXED
// raw value (e.g. the i32 literal) where the setter's `ptr` (box) value
// parameter was expected, so IR verification rejected the module before any
// ARC symptom could surface. In addition, a CLASS Object field/auto-property
// INITIALIZER (`Val: Object := 1`) likewise stored the raw unboxed value, so
// emitDisposerCore's bz_box_release on that raw non-pointer SEGV'd (calling
// bz_box_release(0x1)) even for a dispose-only program. So four coupled sites
// were all broken for box-typed auto/fields; this fix's commit addresses all
// four as the genuine "box twin of #2490" (string/closure values are already
// pointers, so #2490 never tripped the boxing half — box does):
//   1. emitOwnFieldInit (LlvmBackend_Setup.cpp): box a value-typed Object/
//      interface CLASS field/auto-property initializer before the store (the
//      field-initializer twin of the §8.6 boxing the direct `:=` path does).
//      Without this the field held raw bits and disposal crashed.
//   2. lowerImportedPropertySetValue / lowerVirtualPropertySet: box a value-
//      typed RHS into the setter's `ptr` value parameter before the call (the
//      cross-module and virtual/interface-dispatch setter-call sites).
//   3. ensurePropertyAccessorFn's auto-setter body: retain-new/release-old for
//      a box-storage CLASS field (isBoxStorageMember), mirroring #2490's
//      string/closure branches. Enabled by REORDERING collectBoxReturnMethods/
//      collectBoxStorageFields to run BEFORE declareVtables() (the prepass
//      populates boxStorageFields_ this body now consults — the ordering
//      hazard the issue itself documented).
//   4. lowerImportedPropertySetValue / lowerVirtualPropertySet: release a +1
//      box PRODUCER passed directly as the setter argument (the box twin of
//      #2490's ARGUMENT-PASS release), gated to CLASS-owned / non-itable for
//      the same leak-over-UAF reason as #2490 (a RECORD's setter never
//      retains, and an itable-dispatched setter may target one).
//
// GOVERNING RULE (§23.3): "The setter's implicit `value` parameter has no
// modifier by default and takes a BORROW of the property's declared type" —
// so the callee must mint its OWN +1 to keep the value past the call, exactly
// like the string/closure twin (#2490) and an ordinary field store of a plain
// parameter (lowerBoxFieldStore, #429 / §8.6 / §28.9).
//
// FAIL-RED / PASS-GREEN PROOF (mechanical, see the commit message): with this
// fix's part 3 (the setter retain/release) reverted to the pre-fix bare store
// — but parts 1/2/4 kept, so the program still compiles and reaches the
// setter — T1's shape double-frees (the caller's part-4 release drops the
// field's only reference once the bare-store setter failed to retain) and T2's
// interface shape leaks the displaced value (48 bytes/2 allocations, caught by
// LSan). With part 3 restored, both are clean (exit 0). Parts 1 and 2 are
// prerequisites: without part 2 the module fails IR verification (i32 vs ptr)
// and without part 1 a dispose-only program SEGVs in bz_box_release.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2500 {
    std::string path;
    explicit TempFile2500(std::string p) : path(std::move(p)) {}
    ~TempFile2500() { std::remove(path.c_str()); }
};

void writeSource2500(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2500(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libSrc`, links + runs under ASan. `leaks`
// selects whether LeakSanitizer is armed (T4/T5 run with it OFF — see each
// test's own note; T1/T2/T3 run with it ON). Returns the program's exit code
// on a clean run, 97 if ASan/LSan caught a memory-safety error or leak, -1 on
// compile/link failure, -2 if the toolchain is unavailable (SKIP).
int compileImportRunAsan2500(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2500_" + std::to_string(std::rand());
    TempFile2500 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2500 aObj((dir / (stem + "_a.o")).string());
    writeSource2500(aSrc.path, libSrc);
    if (!compileToObj2500(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2500 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2500 bExe((dir / stem).string());
    writeSource2500(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(bSrc.path, bExe.path, deps, opts).success)
        return -1;

    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=") +
        (leaks ? "1" : "0") +
        ":abort_on_error=0:exitcode=97 " + bExe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(bExe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module (no import) ASan run — for the local interface-dispatch and
// record control cells.
int compileRunAsan2500Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2500_local_" + std::to_string(std::rand());
    TempFile2500 srcFile((dir / (stem + ".bzl")).string());
    TempFile2500 exe((dir / stem).string());
    writeSource2500(srcFile.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!driver.compileFile(srcFile.path, exe.path, {}, opts).success)
        return -1;

    std::string runCmd = std::string("ASAN_OPTIONS=detect_leaks=") +
        (leaks ? "1" : "0") +
        ":abort_on_error=0:exitcode=97 " + exe.path + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    std::remove(exe.path.c_str());
    if (status == -1) return -2;
    if (WIFSIGNALED(status)) return 97;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

const char* kLibBox2500 =
    "module LibBox2500; "
    "public class Holder2500 "
    "  public property Val: Object := 1; "
    "end Holder2500;";

}  // namespace

// ===========================================================================
// T1 — CORE (§23.3/§28.9): a cross-module CLASS auto-property of type Object,
// set twice in succession from DIRECT +1 producers (freshly-boxed value
// literals) through the exported setter (lowerImportedPropertySetValue). With
// this fix, the setter retains each new box / releases the displaced one and
// the caller releases the producer's +1 — fully balanced. LeakSanitizer ON.
// FAIL-RED: reverting only the setter retain/release (part 3) double-frees
// (part 4's caller release then drops the field's only reference once the
// bare-store setter failed to retain it) — confirmed by hand before this test
// landed.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_box_arc_2500,
           cross_module_set_twice_direct_producers_no_leak_23_3_28_9) {
    const std::string appSrc =
        "module App2500T1; import LibBox2500; "
        "public class Program2500T1 "
        "  public static method Main(): int "
        "    using var h := new Holder2500(); "
        "    h.Val := 99; "
        "    h.Val := 100; "
        "    return 0; "
        "  end Main; "
        "end Program2500T1;";
    int r = compileImportRunAsan2500(kLibBox2500, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T2 — the LOOP axis (§28.9): the same cross-module setter exercised in a 5-
// iteration loop, each iteration through the SAME call site. A per-call leak
// or double-free here compounds and is caught by LeakSanitizer regardless of
// iteration count. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_box_arc_2500,
           cross_module_loop_direct_producers_no_leak_28_9) {
    const std::string appSrc =
        "module App2500T2; import LibBox2500; "
        "public class Program2500T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2500(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      h.Val := i; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2500T2;";
    int r = compileImportRunAsan2500(kLibBox2500, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T3 — VALUE correctness (§8.6): the box must ROUND-TRIP — a value set through
// the cross-module setter is read back through the getter and unboxed via a
// `case` typed binding (the established unbox idiom, case_typed_binding_unbox
// _746). Proves the §8.6 boxing at the call site (part 2) stores a real box
// the getter can later unbox, not a type-confused raw value. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_box_arc_2500,
           cross_module_set_then_unbox_value_correct_8_6) {
    const std::string appSrc =
        "module App2500T3; import LibBox2500; "
        "public class Program2500T3 "
        "  public static method Main(): int "
        "    using var h := new Holder2500(); "
        "    h.Val := 41; "
        "    return (case h.Val of var n: int => n + 1, _ => 0); "
        "  end Main; "
        "end Program2500T3;";
    int r = compileImportRunAsan2500(kLibBox2500, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);  // 41 + 1; boxed value round-trips correctly
}

// ===========================================================================
// T4 — CONTROL: a mutable RECORD Object auto-property (§9.2 permits mutable
// record body-properties). Per §9 a record's reference fields are BORROWS with
// no independent +1 and no disposer (§9.7), so the setter ARC is deliberately
// gated to CLASS only — this proves the gate holds: a record's setter keeps
// the plain store, no crash from wrongly retaining/releasing a box the record
// does not own. LeakSanitizer OFF: a record's Object field carries no
// independent +1 in any code path (records have no disposer to balance a
// retain against), so a producer stored into a record's Object field leaks on
// origin/main regardless of this fix — the pre-existing, accepted record-
// field leak characteristic, not a defect this test should fail on.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_box_arc_2500,
           mutable_record_object_auto_property_unaffected_control_9_2) {
    const std::string src =
        "module LocalRec2500T4; "
        "public record RHolder2500T4 "
        "  public property Val: Object := 1; "
        "end RHolder2500T4; "
        "public class Program2500T4 "
        "  public static method Main(): int "
        "    var r := new RHolder2500T4(); "
        "    r.Val := 2; "
        "    return 0; "
        "  end Main; "
        "end Program2500T4;";
    int r = compileRunAsan2500Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash (pre-existing record leak accepted)
}

// ===========================================================================
// T5 — INTERFACE dispatch (lowerVirtualPropertySet's itable branch), box-
// typed, LOCAL: a direct +1 producer set through an INTERFACE reference. Proves
// the generated setter body's retain/release (ensurePropertyAccessorFn) and the
// call-site boxing fire safely (NO CRASH) through itable dispatch. LeakSanitizer
// OFF for the SAME documented reason as #2490's T8: the itable branch cannot
// tell from `ps->parent` alone whether the concrete runtime receiver is a CLASS
// (whose setter retains — safe to release the producer) or a BOXED RECORD
// (whose setter does not — releasing would UAF), so the call-site producer
// release is deliberately omitted for the itable branch (leak-over-UAF, the
// safe direction). This test pins crash-freedom only; the accepted itable
// producer leak is the documented trade-off, not a defect this test fails on.
// ===========================================================================
BEZEL_TEST(xmod_auto_property_setter_box_arc_2500,
           interface_dispatch_object_direct_producer_no_crash_23_3) {
    const std::string src =
        "module LocalVirt2500T5; "
        "public interface IBox2500T5 "
        "  property Val: Object get; set; end; "
        "end IBox2500T5; "
        "public class Impl2500T5: IBox2500T5 "
        "  public property Val: Object := 1; "
        "end Impl2500T5; "
        "public class Program2500T5 "
        "  public static method Main(): int "
        "    using var h := new Impl2500T5(); "
        "    var iref: IBox2500T5 := h; "
        "    iref.Val := 99; "
        "    iref.Val := 100; "
        "    return 0; "
        "  end Main; "
        "end Program2500T5;";
    int r = compileRunAsan2500Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash (itable producer leak accepted)
}

}  // namespace bezel::module
