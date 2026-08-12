// fix #2540 (§23.4 "Block-form indexers" / §28.9 "Runtime-managed types" /
// §11 interfaces, v0.236): the indexer twin of #2535's property-side
// named-local-borrow gap. A NAMED LOCAL string passed as the RHS of an
// INDEXER SET through an INTERFACE-typed reference — `iiface[i] := s;` —
// leaked its own independent +1 at the caller's scope end, the same way
// `iiface.Name := s;` leaked before #2535. Two paired edits:
//
// (1) StringEscapeWalker's AssignmentStmt property-SET relaxation
//     (LlvmBackend_Setup.cpp, ~14698 — the `classOrIfacePropSet`
//     discriminant) gains a parallel `classOrIfaceIdxSet` branch that
//     widens the same Class|Interface safe-read relaxation to IndexExpr
//     targets whose indexer PropertySymbol's parent is a CLASS or
//     INTERFACE. The walker now treats `iiface[i] := s;` as a
//     CALL-BOUNDED BORROW when `s` is the tracked local, arming the
//     local's scope-end release. Mirrors the property gate's authority.
//     The IndexExpr branch sits AFTER the property branch (mutually
//     exclusive on LHS shape — a MemberAccessExpr is never also an
//     IndexExpr), so the property gate is unchanged.
//
// (2) lowerIndexerSet's interface-typed dispatch branch
//     (LlvmBackend_Assign.cpp, the `if (recvDecl->kind ==
//     ast::TypeDeclKind::Interface)` block) gains a parallel
//     `classBB`/`recordBB`/`doneBB` block (mirroring #2535's #2535's
//     `lowerVirtualPropertySet` post-call shape in LlvmBackend_Exprs.cpp).
//     On the RECORD-backed runtime path (NULL = plain-store, read from
//     the itable's trailing `ifaceItabSlotCount_[ps->parent]` slot
//     baked in by #2506), `emitStringRetain(val)` fires WHEN the RHS is
//     a tracked STRING NAMED LOCAL (the SAME gating sets the walker uses
//     to decide whether to arm the scope-end release — only those locals
//     get the balancing retain). The class-backed runtime branch is a
//     no-op (the class setter retains → field has +1, local keeps its
//     +1, scope-end release drops the local's +1, sound). Net effect:
//
//       class-backed setter ⇒ rc+1 (setter retains), no call-site retain,
//         scope-end release drops the local's +1 (rc-1). Field has its own
//         +1, local's +1 released soundly. Sound.
//       record-backed setter ⇒ rc unchanged (plain store; field aliases
//         local), call-site retain mints +1 (rc+1), scope-end release
//         drops the local's "original" +1 (rc-1). Field's alias stays
//         alive past the local's scope; field disposal drops the last +1.
//         Sound.
//
// A producer-temporary RHS does NOT get the call-site retain — that would
// leave a stranded +1 (heap rc+1 with no owner to release; only the
// NAMED-LOCAL gate excludes the producer-temporary shape, mirroring #2535's
// `ifaceNamedLocalEligible` predicate).
//
// REGRESSION (T1-T6 below):
//   T1 — CORE: cross-module CLASS-backed interface-typed reference, named
//        local in a 5-iteration loop. The issue's own repro. LSan ON.
//        FAIL-RED on HEAD 1a3a373 (190 bytes / 10 allocations under LSan).
//   T2 — VALUE round-trip: cross-module CLASS-backed interface-typed
//        reference, named local set then read back. LSan ON.
//   T3 — SAME-MODULE CLASS-backed interface-typed reference (the
//        cross-module twin of T1). LSan ON.
//   T4 — CRITICAL — RECORD-implementor named local: a RECORD (not a
//        class) behind the interface, whose indexer SETTER stores the
//        value into a reference-typed field. WITHOUT the paired
//        call-site retain added in this commit (the walker widening alone
//        would arm the local's scope-end release on the record-implementor
//        path), the post-scope field read would UAF — the release would
//        drop the field's only reference. This cell is the CATCH for the
//        paired-retain requirement: with the walker widening alone (W7-A
//        49f8a1b, reverted by the reviewer), T4 would SEGV on read. With
//        the walker widening AND the paired call-site retain (this fix),
//        the call-site retain mints the extra +1 on the record-backed
//        path, so the scope-end release drops soundly and the field stays
//        alive. LSan ON for class-implementor control, OFF for record-
//        implementor (records have no disposer, accepted #2505/T4-style
//        family leak characteristic, not a defect this test should
//        catch — same shape #2535-T4 documents; the test catches
//        correct-and-no-UAF on the field read, exit == recorded length).
//   T5 — CONTROL — CLASS (non-interface) indexer: the leak #2505's gate
//        excluded — the same shape as T1 with the interface removed.
//        Sound under the walker widening (no interface dispatch ⇒ no
//        record-implementor risk, no paired retain needed). LSan ON.
//   T6 — CONTROL — VTABLE/VIRTUAL CLASS PROPERTY: same-module class with
//        a virtual/override property (vtable, NOT itable) set from a
//        named local. The property-SET gate's family, unaffected by this
//        fix's INDEXER-only widening. Proves the indexer-only widening
//        doesn't disturb the class-vtable property path. LSan ON.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile2540 {
    std::string path;
    explicit TempFile2540(std::string p) : path(std::move(p)) {}
    ~TempFile2540() { std::remove(path.c_str()); }
};

void writeSource2540(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2540(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libSrc`, links + runs under ASan. `leaks`
// selects whether LeakSanitizer is armed. Returns the program's exit code
// on a clean run, 97 if ASan/LSan caught a memory-safety error or leak,
// -1 on compile/link failure, -2 if the toolchain is unavailable (SKIP).
int compileImportRunAsan2540(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2540_" + std::to_string(std::rand());
    TempFile2540 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2540 aObj((dir / (stem + "_a.o")).string());
    writeSource2540(aSrc.path, libSrc);
    if (!compileToObj2540(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2540 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2540 bExe((dir / stem).string());
    writeSource2540(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
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
int compileRunAsan2540Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2540_local_" + std::to_string(std::rand());
    TempFile2540 srcFile((dir / (stem + ".bzl")).string());
    TempFile2540 exe((dir / stem).string());
    writeSource2540(srcFile.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, srcFile.path, exe.path, {}, opts).success)
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
// T1 — CORE (§23.4/§28.9/§11, the issue's own repro): a CLASS instance held
// through an INTERFACE-typed reference; a NAMED LOCAL (a +1 producer) is
// stored into the interface's read-write indexer in a 5-iteration loop. Per-
// iteration, loop-compounding leak (190 bytes/10 allocations under LSan on
// HEAD 1a3a373). LeakSanitizer ON.
// FAIL-RED: on HEAD 1a3a373 this leaks under LSan → exit 1.
// PASS-GREEN: with this fix's walker widening + paired call-site retain the
//   class-backed setter retains → field has +1, local keeps its +1,
//   scope-end release drops the local's +1, sound.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           cross_module_iface_class_indexer_named_local_loop_no_leak_23_4) {
    const std::string libSrc =
        "module LibIface2540T1; "
        "public interface IIdx2540T1 "
        "  property [i: int]: string get; set; end; "
        "end IIdx2540T1; "
        "public class Holder2540T1: IIdx2540T1 "
        "  private _v: int := 0; "
        "  public property [i: int]: string "
        "    get => \"read\"; "
        "    set => do _v := value.Length; end; "
        "  end; "
        "end Holder2540T1;";
    const std::string appSrc =
        "module AppIface2540T1; import LibIface2540T1; "
        "public class Program2540T1 "
        "  public static method Main(): int "
        "    using var h := new Holder2540T1(); "
        "    var iref: IIdx2540T1 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      iref[i] := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T1;";
    int r = compileImportRunAsan2540(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — VALUE correctness (§23.4/§8.6): the named local's string ROUND-TRIPS —
// set into the interface-dispatched indexer, then read back through the
// interface getter. Proves the setter reached the implementing class's slot
// AND that arming the local's scope-end release did not free the slot's
// string (the field's OWN +1 stays alive). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           cross_module_iface_class_indexer_named_local_value_roundtrips_23_4) {
    const std::string libSrc =
        "module LibIface2540T2; "
        "public interface IIdx2540T2 "
        "  property [i: int]: string get; set; end; "
        "end IIdx2540T2; "
        "public class Holder2540T2: IIdx2540T2 "
        "  private _v: string := \"init\"; "
        "  public property [i: int]: string "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end Holder2540T2;";
    const std::string appSrc =
        "module AppIface2540T2; import LibIface2540T2; "
        "public class Program2540T2 "
        "  public static method Main(): int "
        "    using var h := new Holder2540T2(); "
        "    var iref: IIdx2540T2 := h; "
        "    var s := \"hel\" + \"lo\"; "
        "    iref[0] := s; "
        "    return iref[0].Length; "
        "  end Main; "
        "end Program2540T2;";
    int r = compileImportRunAsan2540(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length
}

// ===========================================================================
// T3 — SAME-MODULE CLASS-backed interface-typed reference (the cross-module
// twin of T1, broadening the discriminant). The setter retain and the
// itable owner-kind flag are identical whether the interface/class is
// imported or local, so this shape leaked the same per-call +1 before this
// fix. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           same_module_iface_class_indexer_named_local_loop_no_leak_23_4) {
    const std::string src =
        "module LocalIface2540T3; "
        "public interface IIdx2540T3 "
        "  property [i: int]: string get; set; end; "
        "end IIdx2540T3; "
        "public class Holder2540T3: IIdx2540T3 "
        "  private _v: int := 0; "
        "  public property [i: int]: string "
        "    get => \"read\"; "
        "    set => do _v := value.Length; end; "
        "  end; "
        "end Holder2540T3; "
        "public class Program2540T3 "
        "  public static method Main(): int "
        "    using var h := new Holder2540T3(); "
        "    var iref: IIdx2540T3 := h; "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      iref[i] := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T3;";
    int r = compileRunAsan2540Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T4 — CRITICAL — RECORD-implementor named local (§9/§9.7/§11/§23.4/§28.9,
// v0.236): the CATCH cell for the paired-call-site-retain requirement. A
// RECORD is held through an INTERFACE-typed reference; the NAMED LOCAL is
// passed to the interface-dispatched indexer SETTER (`iref[0] := s;`),
// the user-written setter body assigns `value` into the record's string
// field `_v` (record's plain store, §9/§9.7 — a record's reference fields
// are borrows with no independent +1 and no disposer; even an explicit
// `do _v := value; end;` setter is a plain store because the record has
// no disposer to balance a retain). The field then ALIASES the local's
// only reference; if the local's scope-end release (armed by the walker
// widening) fires, the field's only ref is dropped and the post-scope
// field read SEGVs.
//
// This is the exact UAF scenario the W7-A reviewer's WALKER-WIDENING-ONLY
// rejection flagged. The paired call-site retain added in this commit —
// the `ifaceIdxNamedLocalEligible` branch in lowerIndexerSet's itable
// dispatch (the `recordBB` block, mirroring #2535's lowerVirtualPropertySet
// shape) — mints the extra +1 on the RECORD-backed runtime path ONLY,
// pre-balancing the local's scope-end release. With walker + paired
// retain (this fix), the field stays alive past the local's scope and the
// post-scope field read returns the length.
//
// The itable's owner-kind flag is NULL for a record-backed dispatch (set
// up by #2506's append), so the flag read in this commit's itable dispatch
// selects the `recordBB` path and the call-site retain fires. With the
// walker widening ALONE (W7-A 49f8a1b, reverted), T4 would SEGV at the
// `iref[0].Length` field read — the scope-end release drops the field's
// only reference. This cell PASSES with the fix and FAILS (SEGV) without
// it; demonstrably discriminated.
//
// LeakSanitizer OFF: a record's string field carries no independent +1 in
// any code path (records have no disposer to balance a retain against) —
// the pre-existing, accepted record-field-leak characteristic (same shape
// #2505-T4 / #2506-T5 / #2500-T4 document). Not a defect this test should
// fail on. The test catches the SAFETY half: no UAF/crash, value-round-
// trips (the field stores "hello" via the user setter and reads back "hello"
// through the getter).
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           iface_record_indexer_named_local_no_uaf_9_7_23_4) {
    const std::string src =
        "module LocalIface2540T4; "
        "public interface IIdx2540T4 "
        "  property [i: int]: string get; set; end; "
        "end IIdx2540T4; "
        "public record RHolder2540T4: IIdx2540T4 "
        "  public _v: string := \"init\"; "
        "  public property [i: int]: string "
        "    get => _v; "
        "    set => do _v := value; end; "
        "  end; "
        "end RHolder2540T4; "
        "public class Program2540T4 "
        "  public static method Main(): int "
        "    var r := new RHolder2540T4(); "
        "    var iref: IIdx2540T4 := r; "
        "    do "
        "      var s := \"hel\" + \"lo\"; "
        "      iref[0] := s; "
        "    end; "
        "    return iref[0].Length; "
        "  end Main; "
        "end Program2540T4;";
    int r = compileRunAsan2540Local(src, /*leaks=*/false);
    if (r == -2) return;
    // SEGV (signal 11) would be returned as 97 by the harness; a record's
    // pre-existing accepted leak characteristic is OFF, so a -1/0/5 exit is
    // the sound outcome. The SEGV-on-paired-retain-missing case is exit 97
    // because ASan catches the SIGSEGV via abort_on_error=0 and exitcode 97.
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length — no UAF/crash on field read
}

// ===========================================================================
// T5 — CONTROL — CLASS (non-interface) indexer (§23.4/§28.9): a class with a
// read-write indexer, named local set directly (no interface dispatch).
// Verified on HEAD 1a3a373 that this shape leaks the SAME way T1 does —
// #2505's gate explicitly excluded indexers via `!ps->isIndexer`, so the
// class-nor-interface-indexer path was previously neither armed. With this
// fix's walker widening the local's scope-end release is armed, sound for
// the discarding-setter shape (`set => do _v := value.Length; end;` — the
// setter does not alias the local). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           class_indexer_named_local_loop_no_leak_control_23_4) {
    const std::string src =
        "module LocalClass2540T5; "
        "public class Holder2540T5 "
        "  private _v: int := 0; "
        "  public property [i: int]: string "
        "    get => \"read\"; "
        "    set => do _v := value.Length; end; "
        "  end; "
        "end Holder2540T5; "
        "public class Program2540T5 "
        "  public static method Main(): int "
        "    using var h := new Holder2540T5(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      h[i] := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T5;";
    int r = compileRunAsan2540Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — CONTROL — VTABLE/VIRTUAL CLASS PROPERTY (§23.3/§28.9): a class with a
// virtual/override property (vtable, NOT itable) set from a named local.
// This is the #2505 shape, unaffected by this fix's INDEXER-only widening.
// Proves the class-vtable path still arms the scope-end release (setter
// retains via ensurePropertyAccessorFn's class-gated auto-retain → local's
// +1 released soundly) — no regression in the class-vtable property-ARC
// family. LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           vtable_class_property_named_local_loop_no_leak_control_23_3) {
    const std::string src =
        "module LocalVtable2540T6; "
        "public class Base2540T6 "
        "  public virtual property Name: string := \"init\"; "
        "end Base2540T6; "
        "public class Derived2540T6: Base2540T6 "
        "end Derived2540T6; "
        "public class Program2540T6 "
        "  public static method Main(): int "
        "    using var b: Base2540T6 := new Derived2540T6(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      b.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T6;";
    int r = compileRunAsan2540Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
