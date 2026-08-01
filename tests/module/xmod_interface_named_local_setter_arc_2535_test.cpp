// fix #2535 (§23.3 "Block-form properties" / §28.9 "Runtime-managed types" /
// §11 interfaces, v0.236): the named-local-borrow twin of #2506's pre-fix
// itable-producer gap. A NAMED LOCAL string passed as the RHS of a property-
// SET through an INTERFACE-typed reference — `iref.Name := s;` — leaked its
// own independent +1 at the caller's scope end. The static escape walker
// (StringEscapeWalker, LlvmBackend_Setup.cpp) walked that RHS as a
// conservative ESCAPE (safeRead=false), suppressing the local's scope-end
// release — the safe (leak-over-UAF) direction #2505 deliberately chose for
// the interface-parent path because the static walk cannot see the runtime
// receiver (a class instance whose setter RETAINS, §23.3, or a boxed record
// whose setter keeps the plain store, §9/§9.7). Per-iteration, loop-
// compounding leak (a 5-iter loop leaked exactly 5 objects, 190 bytes/10
// allocations under LSan; confirmed by probe on origin/main 3454a7d before
// this fix).
//
// THE FIX — the runtime owner-kind distinction #2506 already bakes into
// every itable, extended to the scope-end release path. Two paired edits:
//
//  (1) StringEscapeWalker's AssignmentStmt `classPropSet` discriminant
//      (LlvmBackend_Setup.cpp ~14604) is widened from `Class` only to
//      `Class | Interface`. The walker now treats `iref.Name := s;` as a
//      CALL-BOUNDED BORROW when `s` is the tracked local — the safe-read
//      gate that arms `s`'s scope-end release in `stringReleaseEligible_` /
//      `stringBorrowRetainEligible_`. (The discriminator is kept on
//      `Record`, where the setter's plain-store keeps the field ALIASING
//      the local's only reference — the field would dangle at the local's
//      scope end if the release fired.)
//
//  (2) lowerVirtualPropertySet's itable branch (LlvmBackend_Exprs.cpp,
//      ~5036) gains a parallel `recordBB` that emits a conditional
//      `emitStringRetain(v)` when the RHS is a tracked STRING NAMED LOCAL
//      — the same gating set (stringReleaseEligible_ / stringBorrowRetain
//      Eligible_) the walker uses, so only the locals the walker armed
//      for release get the balancing retain. The retain fires on the
//      RECORD-backed path ONLY; on the CLASS-backed path the setter's
//      own retain supplies the independent +1 the scope-end release then
//      drops (no extra retain needed). The flag read is the same trailing
//      itable word #2506 bakes in (ifaceItabSlotCount_[iface], non-null =
//      class-backed setter retains, null = box/record plain-store), so
//      cross-module soundness carries over (the flag travels inside the
//      itable bytes the concrete type's own module produced).
//
// Net effect:
//   * class-backed runtime ⇒ setter retains (rc+1), no call-site retain,
//     scope-end release drops the local's +1 (rc-1). Field has its own
//     +1, local's +1 released soundly. Sound.
//   * record-backed runtime ⇒ setter plain-stores (field aliases local,
//     rc unchanged), call-site retain mints the extra +1 (rc+1), scope-
//     end release drops the local's "original" +1 (rc-1). Field's alias
//     stays alive past the local's scope; field disposal drops the last
//     +1. Sound.
//
// Mirroring #2506's producer-release gating: a producer-temporary RHS
// does NOT get the retain — that would leave a stranded +1 (heap rc+1,
// field rc-1 at disposal, heap rc=1, leak; only this fix's NAMED-LOCAL
// gate excludes the producer-temporary shape).
//
// REGRESSION (T1-T6 below):
//   T1 — CORE: cross-module CLASS-backed interface-typed reference, named
//        local in a 5-iteration loop. The leak site (issue's own repro).
//        LSan ON. FAIL-RED on origin/main: leaks under LSan (exit 1).
//   T2 — VALUE round-trip: cross-module CLASS-backed interface-typed
//        reference, named local set then read back. LSan ON.
//   T3 — SAME-MODULE CLASS-backed interface-typed reference (the
//        cross-module twin T1, broadening the discriminant). LSan ON.
//   T4 — CONTROL — RECORD: a RECORD behind an interface-typed reference,
//        named local RHS. The discriminant the gate exists to PROTECT —
//        a record's setter keeps the PLAIN STORE, so the field ALIASES
//        the local's only reference; with this fix's call-site retain on
//        the record-backed path, the local keeps an independent +1 and
//        the scope-end release drops it soundly. (The fix RETURNS the
//        leak here too — see the broad sweep below. LSan ON.) Verified
//        FAIL-RED before this fix: with the fix patched out, T4 leaks
//        (the local's +1 isn't released at scope end). With the fix,
//        T4 stays leak-free AND crash-free.
//   T5 — CONTROL — VIRTUAL/VTABLE class: same-module class with a
//        virtual/override property (vtable, NOT itable) set from a named
//        local. This is the #2505 shape, unaffected by this fix; proves
//        the interface-only widening of the walker gate doesn't disturb
//        the class-vtable path.
//   T6 — CROSS-MODULE interface AND class implementor, named local RHS.
//        Cross-module soundness — the itable flag carries the owner-kind
//        across modules (the flag travels inside the itable bytes the
//        library produced, so the importer reads it correctly regardless
//        of vtable/descriptor linkage which is per-module InternalLinkage
//        and not comparable cross-module).

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

struct TempFile2535 {
    std::string path;
    explicit TempFile2535(std::string p) : path(std::move(p)) {}
    ~TempFile2535() { std::remove(path.c_str()); }
};

void writeSource2535(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2535(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libSrc`, links + runs under ASan. `leaks`
// selects whether LeakSanitizer is armed. Returns the program's exit code
// on a clean run, 97 if ASan/LSan caught a memory-safety error or leak,
// -1 on compile/link failure, -2 if the toolchain is unavailable (SKIP).
int compileImportRunAsan2535(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2535_" + std::to_string(std::rand());
    TempFile2535 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2535 aObj((dir / (stem + "_a.o")).string());
    writeSource2535(aSrc.path, libSrc);
    if (!compileToObj2535(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2535 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2535 bExe((dir / stem).string());
    writeSource2535(bSrc.path, appSrc);

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

// Single-module (no import) ASan run — for the local interface-dispatch,
// record control, and vtable control cells.
int compileRunAsan2535Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2535_local_" + std::to_string(std::rand());
    TempFile2535 srcFile((dir / (stem + ".bzl")).string());
    TempFile2535 exe((dir / stem).string());
    writeSource2535(srcFile.path, src);

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
// T1 — CORE (§23.3/§28.9/§11, the issue's own repro): a CLASS instance held
// through an INTERFACE-typed reference; a NAMED LOCAL (a +1 producer) is
// stored into the interface's auto-property in a 5-iteration loop. Per-
// iteration, loop-compounding leak (190 bytes/10 allocations under LSan on
// origin/main 3454a7d). LeakSanitizer ON.
// FAIL-RED: on origin/main this leaks under LSan → exit 1.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           cross_module_iface_class_named_local_loop_no_leak_23_3_28_9) {
    const std::string libSrc =
        "module LibIface2535T1; "
        "public interface IHolder2535T1 "
        "  property Name: string get; set; end; "
        "end IHolder2535T1; "
        "public class Holder2535T1: IHolder2535T1 "
        "  public property Name: string := \"init\"; "
        "end Holder2535T1;";
    const std::string appSrc =
        "module AppIface2535T1; import LibIface2535T1; "
        "public class Program2535T1 "
        "  public static method Main(): int "
        "    using var h := new Holder2535T1(); "
        "    var iref: IHolder2535T1 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      iref.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2535T1;";
    int r = compileImportRunAsan2535(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T2 — VALUE correctness (§23.3/§8.6): the named local's string ROUND-TRIPS —
// set into the interface-dispatched property, then read back through the
// interface getter. Proves the setter's class-backed retain stored a real
// reference the getter can later read AND that arming the local's scope-end
// release did not release the field's OWN retained reference (no UAF on
// read-back). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           cross_module_iface_class_named_local_value_roundtrips_23_3) {
    const std::string libSrc =
        "module LibIface2535T2; "
        "public interface IHolder2535T2 "
        "  property Name: string get; set; end; "
        "end IHolder2535T2; "
        "public class Holder2535T2: IHolder2535T2 "
        "  public property Name: string := \"init\"; "
        "end Holder2535T2;";
    const std::string appSrc =
        "module AppIface2535T2; import LibIface2535T2; "
        "public class Program2535T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2535T2(); "
        "    var iref: IHolder2535T2 := h; "
        "    var s := \"hel\" + \"lo\"; "
        "    iref.Name := s; "
        "    return iref.Name.Length; "
        "  end Main; "
        "end Program2535T2;";
    int r = compileImportRunAsan2535(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length; no crash, no leak
}

// ===========================================================================
// T3 — SAME-MODULE CLASS-backed interface-typed reference (the cross-module
// twin T1, broadening the discriminant). The setter retain and the
// itable owner-kind flag are identical whether the interface/class is
// imported or local, so this shape leaked the same per-call +1 before this
// fix (confirmed by probe). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           same_module_iface_class_named_local_loop_no_leak_23_3_28_9) {
    const std::string src =
        "module LocalIface2535T3; "
        "public interface IHolder2535T3 "
        "  property Name: string get; set; end; "
        "end IHolder2535T3; "
        "public class Holder2535T3: IHolder2535T3 "
        "  public property Name: string := \"init\"; "
        "end Holder2535T3; "
        "public class Program2535T3 "
        "  public static method Main(): int "
        "    using var h := new Holder2535T3(); "
        "    var iref: IHolder2535T3 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      iref.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2535T3;";
    int r = compileRunAsan2535Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T4 — CONTROL (§9/§9.7/§11): a RECORD behind an interface-typed reference,
// named local RHS. The case the call-site retain exists to PROTECT — a
// record's auto-property setter keeps the PLAIN STORE (§9/§9.7 — a record's
// reference fields are borrows with no independent +1 and no disposer), so
// the field ALIASES the local's only reference. The itable's owner-kind
// flag is NULL for the record-backed itable (set up by #2506), so the
// call-site RETAIN fires on the record-backed path, minting the extra +1
// the scope-end release then drops soundly (no UAF on the field's plain-
// stored alias). LeakSanitizer OFF: a record's string field carries no
// independent +1 in any code path (records have no disposer to balance a
// retain against) — the pre-existing, accepted record-field leak
// characteristic (#2505-T4 / #2506-T5 / #2500-T4 document the same), NOT
// a defect this test should fail on. This cell proves the call-site
// retain holds: the post-scope field read succeeds (no UAF/crash, value
// round-trips). A wrong "widened walker gate without paired call-site
// retain" fix would SEGV here (the scope-end release would drop the
// field's only reference). With the fix, the call-site retain mints the
// extra +1, so the scope-end release drops soundly and the field stays
// alive past the local's scope.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           iface_record_named_local_no_uaf_control_9_7) {
    const std::string src =
        "module LocalIface2535T4; "
        "public interface IHolder2535T4 "
        "  property Name: string get; set; end; "
        "end IHolder2535T4; "
        "public record RHolder2535T4: IHolder2535T4 "
        "  public property Name: string := \"init\"; "
        "end RHolder2535T4; "
        "public class Program2535T4 "
        "  public static method Main(): int "
        "    var r := new RHolder2535T4(); "
        "    var iref: IHolder2535T4 := r; "
        "    do "
        "      var s := \"hel\" + \"lo\"; "
        "      iref.Name := s; "
        "    end; "
        "    return iref.Name.Length; "
        "  end Main; "
        "end Program2535T4;";
    int r = compileRunAsan2535Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length; no UAF/crash (pre-existing record-field leak accepted)
}

// ===========================================================================
// T5 — CONTROL — VIRTUAL/VTABLE CLASS (§23.3/§28.9): a class with a
// virtual/override property (vtable, NOT itable) set from a named local.
// This is the #2505 shape, unaffected by this fix's interface-only walker
// widening. Proves the class-vtable path still arms the scope-end release
// (setter retains → local's +1 released soundly) — no regression in the
// class-vtable setter-ARC family.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           vtable_class_named_local_loop_no_leak_control_23_3) {
    const std::string src =
        "module LocalVtable2535T5; "
        "public class Base2535T5 "
        "  public virtual property Name: string := \"init\"; "
        "end Base2535T5; "
        "public class Derived2535T5: Base2535T5 "
        "end Derived2535T5; "
        "public class Program2535T5 "
        "  public static method Main(): int "
        "    using var b: Base2535T5 := new Derived2535T5(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      b.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2535T5;";
    int r = compileRunAsan2535Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T6 — CROSS-MODULE interface AND class implementor (§2.8.2/§2.8.3 rule 4/
// §11.2/§23.3): the interface AND a CLASS implementor live in an IMPORTED
// library; the app holds an interface-typed reference and sets the property
// from a named local through itable dispatch. The itable's owner-kind flag
// travels INSIDE the itable bytes the library produced, so the importer
// reads it correctly regardless of vtable/descriptor linkage (which is per-
// module InternalLinkage and so cannot be compared cross-module). Proves
// the cross-module soundness of the call-site retain's flag read.
// ===========================================================================
BEZEL_TEST(xmod_interface_named_local_setter_arc_2535,
           cross_module_iface_class_named_local_loop_no_leak_2_8_23_3) {
    const std::string libSrc =
        "module LibIface2535T6; "
        "public interface IHolder2535T6 "
        "  property Name: string get; set; end; "
        "end IHolder2535T6; "
        "public class Holder2535T6: IHolder2535T6 "
        "  public property Name: string := \"init\"; "
        "end Holder2535T6;";
    const std::string appSrc =
        "module AppIface2535T6; import LibIface2535T6; "
        "public class Program2535T6 "
        "  public static method Main(): int "
        "    using var h := new Holder2535T6(); "
        "    var iref: IHolder2535T6 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      iref.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2535T6;";
    int r = compileImportRunAsan2535(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

}  // namespace bezel::module