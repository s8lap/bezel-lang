// fix #5342 cross-module regression test: a FRESH-bodied (`=> expr`) `string`
// property getter on an IMPORTED class leaked the `+1` it produced on EVERY
// read. This file is (b) of #5342; (a) — a `+1` string argument to an imported
// class CONSTRUCTOR — is a separate defect in a separate lane, and none of the
// programs below pass a `+1` string to a constructor, so the two leaks cannot
// contaminate each other's measurement.
//
// §2.5 "library import" / §23.2 "deferred visibility" / §14.1 "owner
// substitution" / §28.9 "fresh-class disposal", v0.313.
//
// ── THE DEFECT ───────────────────────────────────────────────────────────────
//
// `bz$<Class>$<Prop>$propget` (LlvmBackend_Setup.cpp,
// emitExportedPropertyGetters) is a DISPATCH SITE, exactly like a vtable or an
// itable slot: the importer holds no `ast::PropertyDecl` for the property
// (`ModuleLoader` never reconstructs a body-less imported PropertyDecl), so it
// cannot decide producer-vs-borrow by inspecting the body that actually runs.
// The vtable and itable sites both install
// `ensureVirtualStringGetterOwnershipThunk` for exactly that reason, and
// `isPlusOneStringProducer`'s `dynamicSlot` fast path compensates on the call
// side. The `$propget` stub was the only one of the three with NEITHER half: it
// forwarded `ensurePropertyAccessorFn`'s result with a bare `CreateRet`, and
// the importer's body-inspection arm answered null, so EVERY cross-module
// getter read was classified a borrow. Direction-asymmetric consequence:
//
//   * a fresh body (`=> "l" + _f`, a +1 producer) handed the importer a
//     reference nobody would ever release — 400 allocations over 200 reads;
//   * a borrow body (`=> _f`, +0) stayed correct, because the borrow
//     classification happened to match.
//
// Releasing the surplus reference in the stub instead is the UNSOUND direction:
// the returned value is the sole owner of a heap string, so dropping it there
// hands the importer a dangling pointer. The fix is the vtable pair, applied at
// this third site: the stub normalizes the hand-back to a guaranteed +1 (for a
// `string` AND a `string?` return), and `isPlusOneStringProducer` classifies an
// imported-stub read as +1 to match.
//
// ── MEASURED ─────────────────────────────────────────────────────────────────
//
// r23-c28 = 9894b0f0, 200 iterations, LSan (6800 B / 400 allocs per leaking
// row; the 5322 HALT producer's 7600 B included the separate constructor-arg
// leak, which these programs do not contain):
//
//   row                          pre-fix            post-fix
//   impY  `=> "l" + _f` string   6800 B / 400      clean
//   impZ  `=> "l" + _f` string?  6800 B / 400      clean
//   impJ  `=> _f`     string     clean             clean
//   impK  `=> _n + 1` int        clean             clean
//   locY  local twin of impY     clean             clean
//   impQ  `lk?.Br` on impJ       clean             clean   <- SOUNDNESS
//   impR2 interface + producer   clean             clean   <- SOUNDNESS
//
// The three soundness rows matter as much as the two leak rows. `impQ` is the
// `?.` spelling of a cross-module read — it calls the SAME stub, so leaving it
// on the borrow classification while the stub starts retaining would have
// turned a clean shape into a per-read leak. `impR2` is an imported class
// reached through an interface, i.e. the itable-fill path: it installs the
// external stub into a slot, and the slot's own thunk must not double-wrap an
// already-normalized stub. A fix that only stopped the leak would still be
// wrong if it broke either.
//
// Loop-amplified (200 iterations, one receiver) for the reason the codegen
// halves document: a one-shot `Main` is not a reliable LSan oracle, because a
// leftover pointer in a register makes a leaked block read as merely
// reachable. Every string here is built at RUNTIME (`"l" + _f` on a non-empty
// field is never folded from two literals) — a literal is an immortal sentinel
// on which release is a no-op, which would make the probe vacuous. The
// programs also print and the rows check the exact total, so a "fix" that
// stopped producing the strings would not pass.
//
// Generated-code ASan/LSan is enabled on the IMPORTER through
// `CodegenOptions::sanitizeAddress`; the rows assert `exit == 0` under
// `detect_leaks=1:exitcode=99`, so exit 0 IS the leak assertion, and an
// LSan report or a refcount over-release UAF is a failure in its own right
// (asserted separately, so a leak can never be masked by a crash).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::codegen {
namespace {

namespace aot_detail = bezel::test::aot_detail;

const std::filesystem::path& wtDir5342b() {
    return wtScratchDir("wt-5342b-codegen");
}

struct TempFile5342b {
    std::string path;
    explicit TempFile5342b(const std::string& p) : path(p) {}
    ~TempFile5342b() { std::remove(path.c_str()); }
};

void writeSource5342b(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct AsanRun5342b {
    bool builtOk = false;
    bool ran = false;
    int exitCode = -1;
    std::string sanitizerText;
    std::string stdoutText;
};

// Runs `exe` under an explicit LSan oracle. `exitcode=99` is reserved for "the
// sanitizer intervened", so it can never be confused with the 0 every row
// below asserts, and `abort_on_error=0` keeps a redzone error in the captured
// text instead of a signal.
bool spawnWithLeakOracle5342b(const std::string& exe,
                              std::string& sanText, std::string& outText,
                              int& exitCode) {
    auto dir = wtDir5342b();
    std::string stem = unique_stem("bezel_5342b_run_");
    TempFile5342b sanOut((dir / (stem + ".san.txt")).string());
    TempFile5342b stdOut((dir / (stem + ".out.txt")).string());
    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {exe}, stdOut.path, sanOut.path, aot_detail::aotTimeoutSec(), timedOut,
        {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    sanText = aot_detail::slurp(sanOut.path);
    outText = aot_detail::slurp(stdOut.path);
    if (timedOut) return false;
    if (!WIFEXITED(status)) { exitCode = -1; return false; }
    exitCode = WEXITSTATUS(status);
    return true;
}

// A REAL two-module build (§2.4 library artifact + §2.5 import): the library
// with `driver.compileObject` (plain), the app with sanitizeAddress = true.
AsanRun5342b runCrossModule5342b(const std::string& libSrc,
                                 const std::string& appSrc) {
    AsanRun5342b result;
    auto dir = wtDir5342b();
    std::string stem = unique_stem("bezel_5342b_xm_");
    TempFile5342b aSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5342b aObj((dir / (stem + "_lib.o")).string());
    TempFile5342b bSrc((dir / (stem + "_app.bzl")).string());
    TempFile5342b bExe((dir / stem).string());

    writeSource5342b(aSrc.path, libSrc);
    writeSource5342b(bSrc.path, appSrc);

    bezel::Driver driver;
    if (!driver.compileObject(aSrc.path, aObj.path).success) return result;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps,
                                         opts)
             .success)
        return result;
    result.builtOk = true;
    result.ran = spawnWithLeakOracle5342b(bExe.path, result.sanitizerText,
                                          result.stdoutText, result.exitCode);
    return result;
}

// The single-module twin, for the local-getter control row: the same ASan/LSan
// oracle with no import, so the ONLY difference from the pin rows is the
// module boundary itself.
AsanRun5342b runSingleModule5342b(const std::string& src) {
    AsanRun5342b result;
    auto dir = wtDir5342b();
    std::string stem = unique_stem("bezel_5342b_sm_");
    TempFile5342b sSrc((dir / (stem + ".bzl")).string());
    TempFile5342b sExe((dir / stem).string());
    writeSource5342b(sSrc.path, src);

    bezel::Driver driver;
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {},
                                         opts)
             .success)
        return result;
    result.builtOk = true;
    result.ran = spawnWithLeakOracle5342b(sExe.path, result.sanitizerText,
                                          result.stdoutText, result.exitCode);
    return result;
}

// The shared oracle for every row below. `expectedOutput` is the EXACT printed
// total, so a "fix" that stopped producing the strings cannot pass by accident.
void expectClean5342b(const AsanRun5342b& r, const std::string& expectedOutput) {
    BEZEL_EXPECT_TRUE(r.builtOk);
    if (!r.builtOk) return;
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    // Soundness first: an over-release UAF or any other sanitizer finding is a
    // failure regardless of the exit code.
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("refcount over-release") ==
                      std::string::npos);
    // Then the leak assertion itself.
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expectedOutput) != std::string::npos);
}

// ── Library sources ──────────────────────────────────────────────────────────
// `_f` is non-empty, so `"l" + _f` is a genuine runtime concat and never a
// literal-folded sentinel.

// impY: the PIN shape — a fresh-bodied non-nullable `string` getter.
const char* kLibY =
    "module L; "
    "public class LK "
    "  private _f: string; "
    "  public constructor() _f := \"xy\"; end constructor; "
    "  public property Br: string => \"l\" + _f; "
    "end LK;";

// impZ: the PIN shape's NULLABLE twin.
const char* kLibZ =
    "module L; "
    "public class LK "
    "  private _f: string; "
    "  public constructor() _f := \"xy\"; end constructor; "
    "  public property Br: string? => \"l\" + _f; "
    "end LK;";

// impJ: the borrow-bodied control — `=> _f` produces NO +1, so nothing is owed.
const char* kLibJ =
    "module L; "
    "public class LK "
    "  private _f: string; "
    "  public constructor() _f := \"xy\"; end constructor; "
    "  public property Br: string => _f; "
    "end LK;";

// impK: a non-string getter — no ownership to manage at all.
const char* kLibK =
    "module L; "
    "public class LK "
    "  private _n: int; "
    "  public constructor() _n := 1; end constructor; "
    "  public property Br: int => _n + 1; "
    "end LK;";

// impR2: the itable blast-radius control — an imported class reached through an
// interface, with a FRESH-bodied getter. The exporter builds this itable, so
// the slot carries the vtable-path thunk; the importer-side stub never enters.
// Pinned because the `$propget` stub's ABI changed in this fix and this is the
// shape where the two normalizations meet.
const char* kLibR2 =
    "module L; "
    "public interface IN property Nm: string; end IN; "
    "public class LK : IN "
    "  private _f: string; "
    "  public constructor() _f := \"xy\"; end constructor; "
    "  public property Nm: string get => \"l\" + _f; "
    "    set => _f := value; end; "
    "end LK;";

// impS: the STATIC-property soundness control. A static imported property is
// declared by `declareImportedStaticPropertyGetters` and read through
// `importedStaticPropGetterFns_` — a DIFFERENT stub, with a different
// signature (no `this`) and no normalization from this fix. But
// `declareImportedPropertyGetters` carries no static gate, so the same symbol
// ALSO lands in `importedPropGetterFns_`. A classification keyed on that map
// alone therefore claims a +1 the static stub never produced, and `+=` — which
// reads, concatenates and writes back — aborts with "fatal: refcount
// over-release (string)" (exit 134). The `isStaticProperty` gate in both
// classification arms is what keeps this shape a borrow. Pinned because the
// regression is invisible to every other row here: this is the only shape in
// the file whose stub the fix deliberately does NOT normalize.
const char* kLibS =
    "module L; "
    "public class HolderS public v: string := \"a\"; end HolderS; "
    "public class SStr "
    "  private static owned readonly _h: HolderS := new HolderS(); "
    "  public static property Name: string "
    "    get => _h.v; "
    "    set => do _h.v := value; end; "
    "  end; "
    "end SStr;";

// ── App sources ──────────────────────────────────────────────────────────────

// 200 reads x Length 3 (`"l"` + `"xy"` = `"lxy"`) = 600.
const char* kAppY =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      var x: string := lk.Br; "
    "      t := t + x.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// The `?.` spelling of the same read.
const char* kAppQ =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      var x := lk?.Br; "
    "      if x <> null then t := t + x.Length; end if; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// 200 reads, each non-null (the getter never returns null) = 200. Counted,
// not Length-summed, so this row does not depend on the concat width.
const char* kAppZ =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      var x: string? := lk.Br; "
    "      if x <> null then t := t + 1; end if; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// 200 reads x Length 2 = 400 (impJ, impR2).
const char* kAppLen2 =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      t := t + lk.Br.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// 200 reads of an int getter, each 2 = 400.
const char* kAppK =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      t := t + lk.Br; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Through the interface.
const char* kAppR2 =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var n: IN := lk; "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      t := t + n.Nm.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// Read, then `+=` (read + concat + write back), then read again.
const char* kAppS =
    "module M; import L; "
    "public class Program "
    "  public static method Main(): int "
    "    Console.WriteLine(\"str0=\" + SStr.Name); "
    "    SStr.Name += \"!\"; "
    "    Console.WriteLine(\"str2=\" + SStr.Name); "
    "    return 0; "
    "  end Main; "
    "end Program;";

// The LOCAL twin of the pin row: identical getter body, no module boundary.
const char* kSrcLocY =    "module M; "
    "public class LK "
    "  private _f: string; "
    "  public constructor() _f := \"xy\"; end constructor; "
    "  public property Br: string => \"l\" + _f; "
    "end LK; "
    "public class Program "
    "  public static method Main(): int "
    "    using var lk := new LK(); "
    "    var t: int := 0; "
    "    for var i in 0..<200 do "
    "      var x: string := lk.Br; "
    "      t := t + x.Length; "
    "    end; "
    "    Console.WriteLine(t); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace

// =============================================================================
// PIN ROWS — leaked 400 allocations over 200 reads before the fix.
// =============================================================================

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           pin_impY_getter_plusone_string_no_per_read_leak) {
    expectClean5342b(runCrossModule5342b(kLibY, kAppY), "600");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           pin_impZ_getter_plusone_string_nullable_no_per_read_leak) {
    expectClean5342b(runCrossModule5342b(kLibZ, kAppZ), "200");
}

// =============================================================================
// CONTROL ROWS — clean before the fix AND after it. A fix that only silenced
// the two pin rows by over-releasing, or that widened the normalization to a
// shape it does not own, fails here.
// =============================================================================

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_impJ_getter_zero_string_stays_clean) {
    // The borrow-bodied twin of the pin row. If the fix retained on this
    // getter WITHOUT the matching classification, this row would over-release
    // ("fatal: refcount over-release (string)"); if it classified +1 without
    // retaining, it would leak. Only the pair passes.
    expectClean5342b(runCrossModule5342b(kLibJ, kAppLen2), "400");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_impK_getter_int_stays_clean) {
    expectClean5342b(runCrossModule5342b(kLibK, kAppK), "400");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_locY_local_getter_stays_clean) {
    // Same getter body, no module boundary. The vtable/inline path was already
    // correct, so this row pins that the fix did not disturb it.
    expectClean5342b(runSingleModule5342b(kSrcLocY), "600");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_impQ_nullcond_imported_borrow_stays_clean) {
    // `lk?.Br` calls the SAME `$propget` stub as `lk.Br`, so the stub's new
    // retain applies here too. This row is the SOUNDNESS guard for the
    // `?.` classification arm: without it, this shape leaks one reference per
    // read (it was clean before the fix).
    expectClean5342b(runCrossModule5342b(kLibJ, kAppQ), "400");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_impR2_imported_interface_producer_stays_clean) {
    // An imported class reached through an interface: the itable-fill path,
    // where a slot-level thunk and the stub's own normalization meet. Pinned
    // because the stub's ABI changed and this is the shape most able to
    // double-wrap.
    expectClean5342b(runCrossModule5342b(kLibR2, kAppR2), "600");
}

BEZEL_TEST(issue_5342_fresh_bodied_imported_getter_per_read_leak_5342b,
           ctrl_impS_imported_static_property_no_overrelease) {
    // A STATIC imported property read. Its stub is NOT normalized by this fix,
    // so it must stay a borrow: classifying it +1 (which the
    // `importedPropGetterFns_` membership alone would do) aborts the program
    // with a refcount over-release rather than a leak, so this row is the
    // soundness half of the `isStaticProperty` gate.
    AsanRun5342b r = runCrossModule5342b(kLibS, kAppS);
    BEZEL_EXPECT_TRUE(r.builtOk);
    if (!r.builtOk) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("refcount over-release") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("AddressSanitizer") == std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("str0=a") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("str2=a!") != std::string::npos);
}

}  // namespace bezel::codegen
