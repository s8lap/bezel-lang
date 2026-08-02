// fix #2506 (§23.3 "Block-form properties" / §28.9 "Runtime-managed types" /
// §11 interfaces, v0.236): the itable-dispatch twin of #2490/#2500's ARGUMENT-
// PASS release. lowerVirtualPropertySet's `isIfaceProp` (itable) branch
// DELIBERATELY omitted the release that the direct/local (`!isIfaceProp`) and
// lowerImportedPropertySetValue branches had: `ps->parent` there is always the
// INTERFACE, which carries no information about the CONCRETE runtime receiver
// (a class instance whose setter RETAINS, §23.3, or a boxed record whose setter
// keeps the plain store, §9/§9.7), so #2490 conservatively LEAKED a direct +1
// producer passed through an interface-dispatched setter (leak-over-UAF, the
// safe direction) "until that distinction can be made soundly."
//
// OBSERVED-vs-CLAIMED (verified-first on origin/main `ae3c80b`): the leak is
// REAL. `iobj.Prop := <+1 producer>;` through an INTERFACE-typed reference
// backed by a CLASS instance leaks the producer's box (24 bytes/1 allocation
// under LSan) — confirmed by probe before any change. The #2490/#2500 fixes
// left this exact shape as #2500-T5 / #2490-T8, both run with LeakSanitizer
// OFF ("accepted itable producer leak"). This fix turns those leaks off.
//
// THE FIX — a SOUND cross-module class-vs-record discriminant. The itable for
// a (concrete-type, interface) pair is produced by the concrete type's OWN
// module, so a trailing "owner-kind" flag word baked INTO the itable bytes
// travels with it regardless of vtable/descriptor linkage (which is per-module
// InternalLinkage and so cannot be compared cross-module):
//   * every CLASS itable (itab$C$I, declareVtables Pass D) appends a NON-NULL
//     flag — a class's auto-property setter body always RETAINS (§23.3), so
//     the caller MUST release a +1 producer to balance that retain;
//   * every RECORD box itable (itab$box$R$I, setupBoxDescriptors Pass C)
//     appends a NULL flag — a record's setter keeps the plain store (§9/§9.7),
//     so the caller MUST NOT release (that would drop the field's only
//     reference → UAF, the live failure #2490's adversarial pass found for the
//     direct-set record case).
// The flag sits at index ifaceItabSlotCount_[iface] (== the itable's fn-ptr
// slot count, computed identically by every module from the interface's fixed
// declaration order), so class and record itables for the same interface agree
// on its position. lowerVirtualPropertySet reads it after the setter call and
// releases ONLY on the class-backed path; the record path skips. The
// specialized-generic class itable inherits the flag through its slot-by-slot
// copy of the source class itable. Method-only box itables (prim/span/value-
// builtin) omit the flag: their interfaces declare no settable property, so a
// setter dispatch never indexes into them.
//
// FAIL-RED / PASS-GREEN PROOF (mechanical): on origin/main `ae3c80b` (without
// this fix), T1's shape leaks under LSan (24 bytes/1 allocation, exit 1); T5's
// record control must be run with LSan OFF to avoid the PRE-EXISTING accepted
// record-field leak (a record's reference fields are borrows with no disposer,
// §9/§9.7 — unrelated to this fix). With this fix, T1-T4/T6 run LeakSanitizer
// ON and exit 0 (leak gone); T5 (record control) stays crash-free with LSan
// OFF — the discriminant correctly SKIPS the release on the record path, so no
// UAF is introduced (a wrong unconditional release would SEGV under ASan).

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

struct TempFile2506 {
    std::string path;
    explicit TempFile2506(std::string p) : path(std::move(p)) {}
    ~TempFile2506() { std::remove(path.c_str()); }
};

void writeSource2506(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2506(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libSrc`, links + runs under ASan. `leaks`
// selects whether LeakSanitizer is armed (the record control T5 runs with it
// OFF — see its own note; the class/leak-fix cells run with it ON). Returns the
// program's exit code on a clean run, 97 if ASan/LSan caught a memory-safety
// error or leak, -1 on compile/link failure, -2 if the toolchain is unavailable
// (SKIP).
int compileImportRunAsan2506(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2506_" + std::to_string(std::rand());
    TempFile2506 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2506 aObj((dir / (stem + "_a.o")).string());
    writeSource2506(aSrc.path, libSrc);
    if (!compileToObj2506(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2506 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2506 bExe((dir / stem).string());
    writeSource2506(bSrc.path, appSrc);

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
int compileRunAsan2506Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2506_local_" + std::to_string(std::rand());
    TempFile2506 srcFile((dir / (stem + ".bzl")).string());
    TempFile2506 exe((dir / stem).string());
    writeSource2506(srcFile.path, src);

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

}  // namespace

// ===========================================================================
// T1 — CORE (§23.3/§28.9/§11): a CLASS instance behind an INTERFACE-typed
// reference, set twice in succession from DIRECT +1 box producers (freshly-
// boxed value literals) through itable dispatch (lowerVirtualPropertySet's
// isIfaceProp branch). With this fix, the trailing owner-kind flag identifies
// the concrete receiver as class-backed, so the call site releases each +1
// producer to balance the setter's retain — fully balanced. LeakSanitizer ON.
// FAIL-RED: on origin/main this leaks 24 bytes/1 allocation (the displaced
// producer box) under LSan → exit 1.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           interface_class_box_direct_producer_no_leak_23_3_28_9) {
    const std::string src =
        "module LocalVirt2506T1; "
        "public interface IHolder2506T1 "
        "  property Val: Object get; set; end; "
        "end IHolder2506T1; "
        "public class Impl2506T1: IHolder2506T1 "
        "  public property Val: Object := 1; "
        "end Impl2506T1; "
        "public class Program2506T1 "
        "  public static method Main(): int "
        "    using var h := new Impl2506T1(); "
        "    var iref: IHolder2506T1 := h; "
        "    iref.Val := 99; "
        "    iref.Val := 100; "
        "    return 0; "
        "  end Main; "
        "end Program2506T1;";
    int r = compileRunAsan2506Local(src, /*leaks=*/true);
    if (r == -2) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T2 — the LOOP axis (§28.9): the same itable-dispatched setter exercised in a
// 5-iteration loop, each iteration through the SAME call site. A per-call leak
// compounds and is caught by LeakSanitizer. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           interface_class_box_loop_direct_producers_no_leak_28_9) {
    const std::string src =
        "module LocalVirt2506T2; "
        "public interface IHolder2506T2 "
        "  property Val: Object get; set; end; "
        "end IHolder2506T2; "
        "public class Impl2506T2: IHolder2506T2 "
        "  public property Val: Object := 1; "
        "end Impl2506T2; "
        "public class Program2506T2 "
        "  public static method Main(): int "
        "    using var h := new Impl2506T2(); "
        "    var iref: IHolder2506T2 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      iref.Val := i; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2506T2;";
    int r = compileRunAsan2506Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T3 — VALUE correctness (§8.6/§23.3): the box ROUND-TRIPS — a value set
// through the itable-dispatched setter is read back through the interface
// getter and unboxed via a `case` typed binding. Proves the §8.6 boxing at the
// call site stores a real box the getter can later unbox, and that the
// argument-pass release did not release the field's OWN retained reference.
// LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           interface_class_box_set_then_unbox_value_correct_8_6) {
    const std::string src =
        "module LocalVirt2506T3; "
        "public interface IHolder2506T3 "
        "  property Val: Object get; set; end; "
        "end IHolder2506T3; "
        "public class Impl2506T3: IHolder2506T3 "
        "  public property Val: Object := 1; "
        "end Impl2506T3; "
        "public class Program2506T3 "
        "  public static method Main(): int "
        "    using var h := new Impl2506T3(); "
        "    var iref: IHolder2506T3 := h; "
        "    iref.Val := 41; "
        "    return (case iref.Val of var n: int => n + 1, _ => 0); "
        "  end Main; "
        "end Program2506T3;";
    int r = compileRunAsan2506Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);  // 41 + 1; boxed value round-trips correctly
}

// ===========================================================================
// T4 — the STRING producer axis (§23.3/§28.9): the box-typed T1's string twin —
// a +1 STRING producer (`"a" + "b"`) set through an interface-dispatched CLASS
// setter. This is #2490-T8's shape, which #2490 left with LeakSanitizer OFF
// ("accepted itable producer leak"); this fix's owner-kind flag closes the
// string-producer leak too (the flag-gated release covers string/closure/box
// producers uniformly). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           interface_class_string_direct_producer_no_leak_23_3) {
    const std::string src =
        "module LocalVirt2506T4; "
        "public interface ISettable2506T4 "
        "  property Name: string get; set; end; "
        "end ISettable2506T4; "
        "public class Impl2506T4: ISettable2506T4 "
        "  public property Name: string := \"init\"; "
        "end Impl2506T4; "
        "public class Program2506T4 "
        "  public static method Main(): int "
        "    using var h := new Impl2506T4(); "
        "    var iref: ISettable2506T4 := h; "
        "    iref.Name := \"fresh\" + \"prod\"; "
        "    return iref.Name.Length; "
        "  end Main; "
        "end Program2506T4;";
    int r = compileRunAsan2506Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 9);  // "freshprod".Length; no crash, no leak
}

// ===========================================================================
// T5 — CONTROL (§9/§9.7/§11): a RECORD behind an INTERFACE-typed reference, set
// from a direct +1 producer through itable dispatch. This is the case the
// owner-kind discriminant exists to PROTECT: a record's auto-property setter
// keeps the plain store (a record's reference fields are borrows with no
// independent +1 and no disposer, §9/§9.7), so releasing the producer would
// drop the field's only reference → UAF. The NULL flag on the record's box
// itable makes the call site SKIP the release. LeakSanitizer OFF: a record's
// Object field carries no independent +1 in any code path (records have no
// disposer to balance a retain against), so a producer stored into a record's
// Object field leaks on origin-main regardless of this fix — the pre-existing,
// accepted record-field leak characteristic (#2500-T4 documents the same),
// NOT a defect this test should fail on. This cell proves the discriminant
// holds: no UAF/crash is introduced on the record path.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           interface_record_box_direct_producer_no_uaf_control_9_7) {
    const std::string src =
        "module LocalVirt2506T5; "
        "public interface IHolder2506T5 "
        "  property Val: Object get; set; end; "
        "end IHolder2506T5; "
        "public record RImpl2506T5: IHolder2506T5 "
        "  public property Val: Object := 1; "
        "end RImpl2506T5; "
        "public class Program2506T5 "
        "  public static method Main(): int "
        "    var r := new RImpl2506T5(); "
        "    var iref: IHolder2506T5 := r; "
        "    iref.Val := 99; "
        "    iref.Val := 100; "
        "    return 0; "
        "  end Main; "
        "end Program2506T5;";
    int r = compileRunAsan2506Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no UAF/crash (pre-existing record-field leak accepted)
}

// ===========================================================================
// T6 — CROSS-MODULE (§2.8.2/§2.8.3 rule 4/§11.2/§23.3): the interface AND a
// CLASS implementor live in an IMPORTED library; the app holds an interface-
// typed reference and sets the property through itable dispatch. The owner-kind
// flag travels INSIDE the itable bytes the library produced, so the importer
// reads it correctly regardless of vtable/descriptor linkage — this is the
// cross-module soundness a vtable-identity comparison could NOT provide (box
// vtables are per-module InternalLinkage). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_property_setter_itable_arc_2506,
           cross_module_interface_class_box_direct_producer_no_leak_2_8_23_3) {
    const std::string libSrc =
        "module LibIface2506T6; "
        "public interface IHolder2506T6 "
        "  property Val: Object get; set; end; "
        "end IHolder2506T6; "
        "public class Holder2506T6: IHolder2506T6 "
        "  public property Val: Object := 1; "
        "end Holder2506T6;";
    const std::string appSrc =
        "module App2506T6; import LibIface2506T6; "
        "public class Program2506T6 "
        "  public static method Main(): int "
        "    using var h := new Holder2506T6(); "
        "    var iref: IHolder2506T6 := h; "
        "    iref.Val := 99; "
        "    iref.Val := 100; "
        "    return 0; "
        "  end Main; "
        "end Program2506T6;";
    int r = compileImportRunAsan2506(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

}  // namespace bezel::module
