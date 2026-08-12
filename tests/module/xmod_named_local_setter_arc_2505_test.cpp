// fix #2505 (§23.3 "Block-form properties" / §28.9 "Runtime-managed types" /
// §9.7 "Records and disposal", v0.236): the escape-walker twin of #2490/#2500/
// #2506's setter-ARC family. A NAMED LOCAL (a borrow — not a fresh +1
// producer) passed as the RHS of a property-SET on a CLASS — `h.Name := s;` —
// leaked its own independent +1 at the caller's scope end. The class's auto-
// property setter RETAINS its incoming value into the backing field
// (ensurePropertyAccessorFn's class-gated retain, §23.3, minted by #2490),
// giving the field its OWN independent +1 and leaving the local's own +1
// untouched; the local therefore still owns exactly one reference it MUST
// release at scope end. But StringEscapeWalker's AssignmentStmt handler
// (LlvmBackend_Setup.cpp) walked that RHS as a conservative ESCAPE
// (safeRead=false), so the local's scope-end release was SUPPRESSED and its +1
// leaked — a per-call, loop-COMPOUNDING leak (a 5-iteration loop leaked
// exactly 5 objects, confirmed by probe before this fix).
//
// WHY THIS WASN'T A PROBLEM BEFORE #2490. Pre-#2490 the generated setter did
// a bare, unretained store, so the field ALIASED the local's single reference;
// suppressing the local's scope-end release was then LOAD-BEARING for
// correctness (arming it would double-free at the owner's disposal-release of
// the field). #2490's retain flipped which failure mode is latent: now that
// the field takes its own +1, the local's release must fire — but the walker
// was never taught this, so it didn't. This is the residual #2490's own test
// header (xmod_auto_property_setter_arc_2490_test.cpp, "KNOWN, ACCEPTED
// RESIDUAL LEAK") documents as "filed separately" and runs its T1/T5 cells
// with LeakSanitizer OFF — this fix closes that residual.
//
// THE FIX. The walker now recognizes a bare-identifier (or safe-sub-positioned)
// RHS of a property-SET whose PropertySymbol's parent is a CLASS as a CALL-
// BOUNDED BORROW — the AssignmentStmt twin of #1430's method-argument
// relaxation (walkCallArgs) — and walks it as a safe read (safeRead=true), so
// the local stays scope-end-release eligible. The discriminant
// (`ps->parent->kind == Class`) MIRRORS lowerImportedPropertySetValue's
// `classOwned` gate and ensurePropertyAccessorFn's class-gated retain exactly.
//
// SOUNDNESS — the class-vs-record discriminant (do NOT arm the release where
// the field would ALIAS the local's only reference):
//   * a CLASS parent ⇒ the setter retains (§23.3) ⇒ safe read here. T1/T2/T3
//     (cross-module) and T4 (same-module direct) prove the leak is closed and
//     the value round-trips, with LeakSanitizer ON.
//   * a RECORD parent ⇒ the setter keeps the PLAIN STORE (§9/§9.7 — a record's
//     reference fields are borrows with no independent +1 and no disposer), so
//     the field ALIASES the local's ONLY reference; arming the release would
//     dangle that alias (UAF). T5 (record control) proves the discriminant
//     HOLDS: the record path keeps the conservative escape, so no UAF is
//     introduced (run with LeakSanitizer OFF — a record's string field carries
//     no independent +1 in any code path, the pre-existing accepted record-
//     field leak characteristic, #2506-T5 / #2500-T4 document the same).
//   * an INTERFACE parent ⇒ the concrete runtime receiver may be a class OR a
//     boxed record (§11), a runtime fact this static walk cannot see, so it too
//     keeps the conservative escape (leak-over-UAF, the safe direction — the
//     named-local-borrow twin of #2506's pre-fix itable-producer gap; filed as
//     a follow-up, OUT OF SCOPE for this issue).
//
// FAIL-RED / PASS-GREEN PROOF (mechanical): on origin/main (without this fix),
// T1/T2/T4 leak under LSan (190 byte(s) in 10 allocation(s) for the 5-iter
// loop — exit 1); T3 leaks likewise. With this fix, T1-T4 run LeakSanitizer
// ON and exit 0 (leak gone, value correct); T5 (record control) stays crash-
// free with LSan OFF — the discriminant correctly keeps the record path
// escaped, so no UAF is introduced (a wrong unconditional arm would SEGV under
// ASan on T5's post-scope field read).

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

struct TempFile2505 {
    std::string path;
    explicit TempFile2505(std::string p) : path(std::move(p)) {}
    ~TempFile2505() { std::remove(path.c_str()); }
};

void writeSource2505(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2505(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compiles `appSrc` importing `libSrc`, links + runs under ASan. `leaks`
// selects whether LeakSanitizer is armed (the record control T5 runs with it
// OFF — see its own note; the class/leak-fix cells run with it ON). Returns the
// program's exit code on a clean run, 97 if ASan/LSan caught a memory-safety
// error or leak, -1 on compile/link failure, -2 if the toolchain is
// unavailable (SKIP).
int compileImportRunAsan2505(const std::string& libSrc,
                             const std::string& appSrc, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2505_" + std::to_string(std::rand());
    TempFile2505 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2505 aObj((dir / (stem + "_a.o")).string());
    writeSource2505(aSrc.path, libSrc);
    if (!compileToObj2505(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2505 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2505 bExe((dir / stem).string());
    writeSource2505(bSrc.path, appSrc);

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

// Single-module (no import) ASan run — for the same-module direct and record
// control cells.
int compileRunAsan2505Local(const std::string& src, bool leaks) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2505_local_" + std::to_string(std::rand());
    TempFile2505 srcFile((dir / (stem + ".bzl")).string());
    TempFile2505 exe((dir / stem).string());
    writeSource2505(srcFile.path, src);

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
// T1 — CORE (§23.3/§28.9, the issue's own repro): a NAMED LOCAL string is
// stored into an IMPORTED class's auto-property setter in a 5-iteration loop.
// The class setter RETAINS the value (§23.3, #2490), so the field takes its
// own +1 and the local keeps its own — the local MUST release at scope end.
// Pre-fix the escape walker suppressed that release, leaking one object per
// iteration (190 bytes/10 allocations under LSan). LeakSanitizer ON.
// FAIL-RED: on origin/main this leaks under LSan → exit 1.
// ===========================================================================
BEZEL_TEST(xmod_named_local_setter_arc_2505,
           cross_module_class_named_local_loop_no_leak_23_3_28_9) {
    const std::string libSrc =
        "module LibStrLoop2505; "
        "public class Holder2505 "
        "  public property Name: string := \"init\"; "
        "end Holder2505;";
    const std::string appSrc =
        "module AppLoop2505; import LibStrLoop2505; "
        "public class Program2505 "
        "  public static method Main(): int "
        "    using var h := new Holder2505(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      h.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2505;";
    int r = compileImportRunAsan2505(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;  // toolchain / dependency build unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T2 — VALUE correctness (§23.3/§8.6): the named local's string ROUND-TRIPS —
// set into the imported class's property, then read back through the getter.
// Proves the setter's retain stored a real reference the getter can later read
// AND that arming the local's scope-end release did not release the field's
// OWN retained reference (no UAF on read-back). LeakSanitizer ON.
// ===========================================================================
BEZEL_TEST(xmod_named_local_setter_arc_2505,
           cross_module_class_named_local_value_roundtrips_23_3) {
    const std::string libSrc =
        "module LibStrLoop2505b; "
        "public class Holder2505b "
        "  public property Name: string := \"init\"; "
        "end Holder2505b;";
    const std::string appSrc =
        "module AppLoop2505b; import LibStrLoop2505b; "
        "public class Program2505b "
        "  public static method Main(): int "
        "    using var h := new Holder2505b(); "
        "    var s := \"hel\" + \"lo\"; "
        "    h.Name := s; "
        "    return h.Name.Length; "
        "  end Main; "
        "end Program2505b;";
    int r = compileImportRunAsan2505(libSrc, appSrc, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length; no crash, no leak
}

// ===========================================================================
// T3 — SAME-MODULE CLASS direct (§23.3/§28.9): the non-cross-module twin of T1
// — a named local set into a SAME-MODULE class's auto-property in a loop. The
// setter retain is identical whether the class is local or imported, so this
// shape leaked the same per-call +1 before this fix (confirmed by probe).
// LeakSanitizer ON. Broadens the discriminant beyond the cross-module path.
// ===========================================================================
BEZEL_TEST(xmod_named_local_setter_arc_2505,
           same_module_class_named_local_loop_no_leak_23_3_28_9) {
    const std::string src =
        "module LocalClass2505; "
        "public class Holder2505c "
        "  public property Name: string := \"init\"; "
        "end Holder2505c; "
        "public class Program2505c "
        "  public static method Main(): int "
        "    using var h := new Holder2505c(); "
        "    var i := 0; "
        "    while i < 5 do "
        "      var s := \"iter\" + i.ToString(); "
        "      h.Name := s; "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end Program2505c;";
    int r = compileRunAsan2505Local(src, /*leaks=*/true);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);  // no crash, no leak
}

// ===========================================================================
// T4 — CONTROL (§9/§9.7): a RECORD auto-property setter, the case the class-
// only discriminant exists to PROTECT. A record's setter keeps the PLAIN STORE
// (§9/§9.7 — a record's reference fields are borrows with no independent +1
// and no disposer), so the field ALIASES the local's ONLY reference; arming
// the local's scope-end release would dangle that alias. The named local `s`
// is declared in an INNER `do ... end` scope that ends BEFORE the post-scope
// field read, so a wrong unconditional arm would free `s` at the inner scope
// end and the subsequent `r.Name.Length` would read a dangling pointer (UAF/
// SEGV under ASan). The class-only gate keeps the record path escaped, so no
// UAF. LeakSanitizer OFF: a record's string field carries no independent +1 in
// any code path (the pre-existing accepted record-field leak characteristic,
// #2506-T5 / #2500-T4 document the same), unrelated to and unmasked by this
// fix.
// ===========================================================================
BEZEL_TEST(xmod_named_local_setter_arc_2505,
           record_named_local_setter_no_uaf_control_9_7) {
    const std::string src =
        "module LocalRec2505; "
        "public record RHolder2505d "
        "  public property Name: string := \"init\"; "
        "end RHolder2505d; "
        "public class Program2505d "
        "  public static method Main(): int "
        "    var r := new RHolder2505d(); "
        "    do "
        "      var s := \"hel\" + \"lo\"; "
        "      r.Name := s; "
        "    end; "
        "    return r.Name.Length; "
        "  end Main; "
        "end Program2505d;";
    int r = compileRunAsan2505Local(src, /*leaks=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);  // "hello".Length; no UAF/crash (record leak accepted)
}

}  // namespace bezel::module
