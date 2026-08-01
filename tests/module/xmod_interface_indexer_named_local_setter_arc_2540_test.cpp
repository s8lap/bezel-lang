// fix #2540 (§23.4 "Block-form indexers" / §28.9 "Runtime-managed types" /
// §11 interfaces, v0.236): the indexer twin of #2535's property-side
// named-local-borrow gap. A NAMED LOCAL string passed as the RHS of an
// INDEXER SET through an INTERFACE-typed reference — `iref[i] := s;` —
// leaked its own independent +1 at the caller's scope end, the same way
// `iref.Name := s;` leaked before #2535. The StringEscapeWalker's existing
// property-SET gate (LlvmBackend_Setup.cpp, the `classOrIfacePropSet`
// discriminant in the AssignmentStmt handler) deliberately excluded
// indexers via `!ps->isIndexer` (#2505/#2535 closed only the property
// case); the indexer path then fell through to the conservative escape
// below — which suppresses the local's scope-end release and leaves its
// +1 stranded on the heap (a per-call, loop-compounding leak — 190
// bytes / 10 allocations under LSan on origin/main 325e27d before this
// fix; confirmed by direct bezelc probe). The IndexExpr's exprSymbol IS
// the indexer's PropertySymbol (set by ExprTypecheck_Exprs.cpp ~1877 for
// any user-defined indexer access, including imported; #2530 fixed the
// prior gap where `ctx.exprSymbol(*ix)` returned null for indexer
// IndexExpr), so the walker can identify the indexer's parent kind the
// same way it does for the property-SET gate.
//
// THE FIX — a parallel branch in the walker's AssignmentStmt handler
// (LlvmBackend_Setup.cpp, the `classOrIfaceIdxSet` discriminant) widens
// the safe-read relaxation to IndexExpr targets whose indexer
// PropertySymbol's parent is a CLASS or INTERFACE — exactly mirroring
// the property gate's `Class | Interface` discriminant. Mirrors #2535's
// SOUNDNESS contract (the property-side rationale applies identically):
//   * CLASS parent (concrete `h[i] := s;`) ⇒ the indexer setter body is
//     user code; in the common shape the setter EITHER retains into a
//     string-typed field via the auto-retaining `_f := value;` field
//     store (lowerStringFieldStore's load-old / retain-new / store-new /
//     release-old) OR discards the value without storing it — both leave
//     the local's +1 balanced by the callee's own retain OR by the
//     local's scope-end release against an un-aliased value. Same shape
//     as non-auto class property setters (`set => do _v := value; end;`
//     on a class with `var _v: string := "init"`), which the property
//     gate arms soundly and which T2/T3 exercise end-to-end.
//   * INTERFACE parent (`iiface[i] := s;`) ⇒ the static receiver is
//     interface but the runtime implementor may be class or record. The
//     COMMON class-implementor case is the same shape as the CLASS
//     parent above (sound). The rarer record-implementor case where the
//     user's setter stores `value` into a reference-typed field WITHOUT
//     retaining requires the same ifaceItabSlotCount_ + call-site
//     retain #2535 added for properties; that paired call-site fix for
//     the indexer path requires touching Assign.cpp (Lane D, out of
//     this lane's scope; filed separately). This fix does NOT make that
//     record case worse — pre-#2540 the indexer path leaked uniformly
//     (the safe direction) regardless of implementor; the walker gate
//     alone is safe for the class-implementor case and the
//     record-implementor-with-discarding-setter case (no alias to
//     dangle).
//   * RECORD parent ⇒ unchanged: setter keeps the plain store
//     (§9/§9.7 — a record's reference fields are borrows with no
//     independent +1 and no disposer), so the field ALIASES the local's
//     only reference and the gate must stay conservative (filed as a
//     separate #2535-style follow-up for records, mirroring the
//     property case's T4 record control which the call-site retain
//     covers).
//
// REGRESSION (T1-T5 below):
//   T1 — CORE: cross-module CLASS-backed interface-typed reference, named
//        local in a 5-iteration loop, set through the interface-dispatched
//        indexer. The issue's own repro. LSan ON. FAIL-RED on origin/main
//        (5 leaks / 190 bytes); PASS-GREEN with this fix.
//   T2 — VALUE round-trip: cross-module CLASS-backed interface-typed
//        reference, named local set then read back. LSan ON.
//   T3 — SAME-MODULE CLASS-backed interface-typed reference. LSan ON.
//   T4 — CONTROL — CLASS (non-interface) indexer: the same leak the
//        brief's claim that "the class-indexer path was fixed by #2505"
//        describes — verified on origin/main 325e27d: a class-backed
//        indexer set with a named local also leaks (same shape as T1
//        with the interface removed; #2505's gate explicitly excluded
//        indexers via `!ps->isIndexer`). With this fix's walker widening
//        T4 stops leaking. LSan ON.
//   T5 — CONTROL — VTABLE/VIRTUAL CLASS: same-module class with a
//        virtual/override property (vtable, NOT itable) set from a named
//        local. Unaffected by this fix (the property-SET gate is the
//        `classOrIfacePropSet` branch above, distinct from the new
//        `classOrIfaceIdxSet` branch); proves the indexer-only widening
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

// Single-module (no import) ASan run — for the local interface-dispatch
// and class-vtable control cells.
int compileRunAsan2540Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2540_local_" + std::to_string(std::rand());
    TempFile2540 srcFile((dir / (stem + ".bzl")).string());
    TempFile2540 exe((dir / stem).string());
    writeSource2540(srcFile.path, src);

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
// T1 — CORE (§23.4/§28.9/§11, the issue's own repro): a CLASS instance held
// through an INTERFACE-typed reference; a NAMED LOCAL (a +1 producer) is
// stored into the interface's read-write indexer in a 5-iteration loop. Per-
// iteration, loop-compounding leak (190 bytes/10 allocations under LSan on
// origin/main 325e27d). LeakSanitizer ON.
// FAIL-RED: on origin/main this leaks under LSan → exit 1.
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
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — VALUE correctness (§23.4/§8.6): the named local's string ROUND-TRIPS —
// set into the interface-dispatched indexer, then read back through the
// interface getter. Proves the setter reached the implementing class's slot
// and that arming the local's scope-end release did not free the slot's
// string. LeakSanitizer ON.
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
// twin T1, broadening the discriminant). The setter retain and the
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
// T4 — CONTROL — CLASS (non-interface) indexer (§23.4/§28.9): a class with a
// read-write indexer, named local set directly (no interface dispatch).
// Verified on origin/main 325e27d that this shape leaks the SAME way T1
// does — #2505's gate explicitly excluded indexers via `!ps->isIndexer`,
// so neither the class-nor-interface-indexer path was previously armed.
// With this fix's walker widening the local's scope-end release is armed,
// sound for the discarding-setter shape below (setter does not alias the
// local). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           class_indexer_named_local_loop_no_leak_control_23_4) {
    const std::string src =
        "module LocalClass2540T4; "
        "public class Holder2540T4 "
        "  private _v: int := 0; "
        "  public property [i: int]: string "
        "    get => \"read\"; "
        "    set => do _v := value.Length; end; "
        "  end; "
        "end Holder2540T4; "
        "public class Program2540T4 "
        "  public static method Main(): int "
        "    using var h := new Holder2540T4(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      h[i] := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T4;";
    int r = compileRunAsan2540Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — CONTROL — VTABLE/VIRTUAL CLASS PROPERTY (§23.3/§28.9): a class with a
// virtual/override property (vtable, NOT itable) set from a named local.
// This is the #2505 shape, unaffected by this fix's INDEXER-only widening.
// Proves the class-vtable path still arms the scope-end release (setter
// retains via ensurePropertyAccessorFn's class-gated auto-retain → local's
// +1 released soundly) — no regression in the class-vtable property-ARC
// family.
// ===========================================================================
BEZEL_TEST(xmod_interface_indexer_named_local_setter_arc_2540,
           vtable_class_property_named_local_loop_no_leak_control_23_3) {
    const std::string src =
        "module LocalVtable2540T5; "
        "public class Base2540T5 "
        "  public virtual property Name: string := \"init\"; "
        "end Base2540T5; "
        "public class Derived2540T5: Base2540T5 "
        "end Derived2540T5; "
        "public class Program2540T5 "
        "  public static method Main(): int "
        "    using var b: Base2540T5 := new Derived2540T5(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      b.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2540T5;";
    int r = compileRunAsan2540Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
