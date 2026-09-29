// fix #4650 (§9.2 "Records" / §2.5 "Cross-module symbols" / §8.3 "Equality" /
// §2.8.3 rule 3, v0.307) — an IMPORTED body record whose auto-property is
// declared AFTER any other symbol-producing member DROPS that property's
// constructor store, with the #4045 backstop's `internal:` Warning and a
// RUNNING binary whose field was never initialised.
//
// WHERE THE BUG WAS. `ModuleLoader::parseAndAttachCtorBody` re-parses the
// exporter's `ctorBodyText` over a synthetic single-type source and then
// remaps every fresh symbol in that body back to the IMPORTED symbol, so
// codegen's `storageMemberOf` / `fieldIndex_` / `recordStruct` stay keyed on
// the imported type (#3207). The auto-property half of that remap is driven
// by `importedAutoPropStorage`, a name→imported-PropertySymbol map built by
// SYNC-WALKING the wire's `td.members` against `importedType.members`
// position-by-position. Every other sync-walk in that file advances its index
// on `metaMemberProducesSymbol(mm)`; this one — and its copy in
// `parseAndAttachMethodBody` (#3994) — advanced only on
// `mm.kind == MetaMemberKind::Property`, `continue`-ing past every other
// member WITHOUT consuming its slot. The pairing therefore desynchronised by
// the number of non-Property members declared BEFORE the property.
//
// `metaMemberProducesSymbol`'s own comment states the contract this violated,
// verbatim: "every call site here sync-walks td.members against
// importedType.members position-by-position … a stale `false` here would
// desync that walk by one".
//
// WHAT THE DEFECT WAS, MEASURED on `f644098f` (= origin/main) with a
// two-module `bezelc -c lib.bzl -o lib.o` + `bezelc --import lib.o app.bzl`
// AOT pair — the strict oracle:
//
//   module LibCtlA;
//   public record Counter
//     private _count: int;                       -- declared FIRST
//     public readonly property Label: string;
//     public constructor(label: string, count: int)
//       Label := label;
//       _count := count;
//     end constructor;
//   end Counter;
//
// `_count` was skipped without advancing, so the Property `Label` paired
// against `importedType.members[0]` — the FieldSymbol `_count`. The
// `dynamic_cast<sema::PropertySymbol*>` failed, the map came out EMPTY, the
// remap arm found no imported twin for the fresh `Label`, and NO `mainCtx`
// exprSymbol entry was written for it at all. codegen's `implicitFieldAddr`
// then got a null symbol back from `ctx_.exprSymbol`, `lvalueAddr` returned
// null, and the #4045 backstop reported
//
//   internal: assignment target lvalue address failed to resolve
//     (store dropped — target retains its previous value) — #4045
//
// at Warning severity and LET THE COMPILE CONTINUE — producing a runnable
// binary whose `Label` was never stored.
//
// Declaring the auto-property BEFORE the field masked it exactly (index 0
// then paired correctly), which is why #3207's own fixture — auto-property
// only, no explicit field — has always passed. T5 below pins that masking
// shape so the fix cannot be "corrected" back into it.
//
// THE THREE OBSERVABLE FACES, all measured pre-fix:
//
//   T1  string-typed auto-property, read back through `+` concatenation
//       PRE-FIX : SIGSEGV (the never-stored `bz_string` is dereferenced)
//       POST-FIX: exit 0, "Label=[x] count=1"
//
//   T2  int-typed auto-property — no pointer to dereference, so the binary
//       runs to completion with a SILENT WRONG VALUE
//       PRE-FIX : exit 0, "Tag=0 Count=7"     <-- Tag should be 42
//       POST-FIX: exit 0, "Tag=42 Count=7"
//
//   T3  §8.3 content comparison over an `equality skip` sibling field — the
//       shape the issue was filed on. Both `Label`s are the zero-filled null
//       pointer pre-fix, so the auto-`=` answers TRUE for two records whose
//       labels genuinely differ, and `GetHashCode` then dereferences the
//       null string.
//       PRE-FIX : "ab=True" (WRONG), "ac=True", then SIGSEGV
//       POST-FIX: exit 0, "ab=False", "ac=True", "hac=True"
//
// ⚠ WHY EVERY ROW ASSERTS ON VALUE, NOT ON "did it compile". The pre-fix
// compiler PRODUCES A BINARY for every one of these programs — the backstop
// is a Warning, not an Error. A row that only checked "the app built" or
// `rc != 139` would pass on both sides of the fix for T2, which is the
// silent-wrong-value face. T1/T3 additionally pin `rc == 0`, which fails on
// the pre-fix SIGSEGV (the harness maps a signal to 1000 + signo, so the
// masked-exit-status trap does not apply here).
//
// ⚠ EACH Tn/Tnb PAIR IS THE DIFFERENTIAL CONTROL. Tn alone would pass on a
// compiler that happens to emit the right constant; Tnb alone would pass on
// a §9.2 enforcement bug. Together they pin "the importer agrees with the
// single-module twin", which is what makes this a cross-module claim.
//
// ⚠ T4 IS THE OFF-BY-N ROW. Two explicit fields precede two auto-properties,
// so the desync is TWO, not one. A fix that advanced the index by a single
// slot (or that special-cased "one preceding field") would pass T1–T3 and
// fail T4.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4650 {
    std::string path;
    explicit TempFile4650(std::string p) : path(std::move(p)) {}
    ~TempFile4650() { std::remove(path.c_str()); }
};

void writeSource4650(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunResult4650 {
    int rc;
    std::string stdout_text;
};

// Drain a child's stdout and collect its status. A SIGNALLED child is
// reported as 1000 + signo so a SIGSEGV can never be confused with an
// ordinary exit status (§ the 8-bit-truncation trap in CLAUDE.md).
RunResult4650 execCapture4650(const std::string& exe) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return {-1, ""};
    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        ::execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0)
        out.append(buf, buf + n);
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return {1000 + WTERMSIG(status), out};
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, out};
}

// Compile the dependency, compile the app against it, run the app in a
// SEPARATE process (per tests/README.md). Returns the child's exit status and
// captured stdout — the only way to assert on program VALUE.
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
RunResult4650 runImported4650(const std::string& libSrc,
                              const std::string& appSrc,
                              const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4650_" + tag + "_" + unique_stem("");

    TempFile4650 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4650 aObj((dir / (stem + "_a.o")).string());
    writeSource4650(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile4650 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4650 bExe((dir / (stem + "_b")).string());
    writeSource4650(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return {-1, ""};
    if (!std::ifstream(bExe.path).good()) return {-1, ""};
    return execCapture4650(bExe.path);
}

// Compile and run a SINGLE-module program — the differential control twin.
RunResult4650 runSingleModule4650(const std::string& src,
                                  const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4650ctl_" + tag + "_" + unique_stem("");
    TempFile4650 sSrc((dir / (stem + ".bzl")).string());
    TempFile4650 sExe((dir / stem).string());
    writeSource4650(sSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {})
             .success)
        return {-1, ""};
    if (!std::ifstream(sExe.path).good()) return {-1, ""};
    return execCapture4650(sExe.path);
}

bool contains4650(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// The record declarations under test. In every one of them the auto-property
// is declared AFTER at least one other symbol-producing member — the shape
// that desynchronised the sync-walk. T5's control inverts the order.
// ---------------------------------------------------------------------------

// T1: explicit field FIRST, then a string-typed auto-property.
const char* kDeclsString4650 =
    "public record Counter "
    "  private _count: int; "
    "  public readonly property Label: string; "
    "  public constructor(label: string, count: int) "
    "    Label := label; "
    "    _count := count; "
    "  end constructor; "
    "  public method Count(): int => _count; "
    "end Counter;";

// T2: the same shape with an INT-typed auto-property — no pointer to
// dereference, so pre-fix the binary ran to completion with `Tag=0`.
const char* kDeclsInt4650 =
    "public record Counter "
    "  private _count: int; "
    "  public readonly property Tag: int; "
    "  public constructor(tag: int, count: int) "
    "    Tag := tag; "
    "    _count := count; "
    "  end constructor; "
    "  public method Count(): int => _count; "
    "end Counter;";

// T3: §8.3 auto-equality over an `equality skip` sibling — the issue's own
// SIGSEGV shape. `_count` is excluded from `=`, so the comparison is
// content-over-`Label` alone.
const char* kDeclsEqSkip4650 =
    "public record Counter "
    "  private equality skip _count: int; "
    "  public readonly property Label: string; "
    "  public constructor(label: string, count: int) "
    "    Label := label; "
    "    _count := count; "
    "  end constructor; "
    "end Counter;";

// T4: TWO explicit fields before TWO auto-properties — the desync is 2.
const char* kDeclsMulti4650 =
    "public record Multi "
    "  private _a: int; "
    "  private _b: int; "
    "  public readonly property P: int; "
    "  public readonly property Q: string; "
    "  public constructor(a: int, b: int, p: int, q: string) "
    "    _a := a; "
    "    _b := b; "
    "    P := p; "
    "    Q := q; "
    "  end constructor; "
    "  public method Sum(): int => _a + _b + P; "
    "end Multi;";

// T5 control: the auto-property declared FIRST — index 0 paired correctly
// even pre-fix, so this row passed on BOTH sides. It is here so the fix
// cannot regress the shape that always worked.
const char* kDeclsPropFirst4650 =
    "public record Counter "
    "  public readonly property Label: string; "
    "  private _count: int; "
    "  public constructor(label: string, count: int) "
    "    Label := label; "
    "    _count := count; "
    "  end constructor; "
    "  public method Count(): int => _count; "
    "end Counter;";

std::string libSource4650(const char* decls, const std::string& tag) {
    return "module BrLib4650" + tag + "; " + decls;
}

std::string appSource4650(const std::string& body, const std::string& tag) {
    return "module BrApp4650" + tag + "; import BrLib4650" + tag + "; " + body;
}

std::string sameModule4650(const char* decls, const std::string& body,
                           const std::string& tag) {
    return "module Ctl4650" + tag + "; " + std::string(decls) + " " + body;
}

// The app bodies, shared verbatim between each imported row and its
// single-module twin so the ONLY difference between the pair is the module
// boundary.
const char* kBodyString4650 =
    "public class C4650S "
    "  public static method Main(): int "
    "    var a := new Counter(\"x\", 1); "
    "    Console.WriteLine(\"Label=[\" + a.Label + \"] count=\" "
    "                      + a.Count().ToString()); "
    "    return 0; "
    "  end Main; "
    "end C4650S;";

const char* kBodyInt4650 =
    "public class C4650I "
    "  public static method Main(): int "
    "    var a := new Counter(42, 7); "
    "    Console.WriteLine(\"Tag=\" + a.Tag.ToString() + \" Count=\" "
    "                      + a.Count().ToString()); "
    "    return 0; "
    "  end Main; "
    "end C4650I;";

const char* kBodyEqSkip4650 =
    "public class C4650E "
    "  public static method Main(): int "
    "    var a := new Counter(\"x\", 1); "
    "    var b := new Counter(\"y\", 2); "
    "    var c := new Counter(\"x\", 9); "
    "    Console.WriteLine(\"ab=\" + (a = b).ToString()); "
    "    Console.WriteLine(\"ac=\" + (a = c).ToString()); "
    "    Console.WriteLine(\"hac=\" "
    "                      + (a.GetHashCode() = c.GetHashCode()).ToString()); "
    "    return 0; "
    "  end Main; "
    "end C4650E;";

const char* kBodyMulti4650 =
    "public class C4650M "
    "  public static method Main(): int "
    "    var m := new Multi(1, 2, 30, \"qq\"); "
    "    Console.WriteLine(\"P=\" + m.P.ToString() + \" Q=[\" + m.Q "
    "                      + \"] Sum=\" + m.Sum().ToString()); "
    "    return 0; "
    "  end Main; "
    "end C4650M;";

}  // namespace

// ===========================================================================
// T1 — HEADLINE ROW. An imported §9.2 body record declaring an explicit field
// BEFORE a string-typed auto-property: the constructor's `Label := label;`
// store must land, and the importer must read back the stored content.
//
// PRE-FIX (measured, f644098f): the #4045 backstop dropped the store, the app
// still linked, and the read-back `+` concatenation dereferenced the
// never-stored `bz_string` — SIGSEGV (rc 1011 through this harness).
// POST-FIX: exit 0, "Label=[x] count=1".
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t1_imported_string_autoprop_after_field_store_lands_9_2) {
    RunResult4650 r = runImported4650(
        libSource4650(kDeclsString4650, "S"),
        appSource4650(kBodyString4650, "S"), "t1");
    if (r.rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "Label=[x] count=1"));
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (differential control).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t1b_same_module_string_autoprop_after_field_store_lands_9_2) {
    RunResult4650 r = runSingleModule4650(
        sameModule4650(kDeclsString4650, kBodyString4650, "S"), "t1b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "Label=[x] count=1"));
}

// ===========================================================================
// T2 — THE SILENT-WRONG-VALUE FACE. The same shape with an `int` auto-property
// has no pointer to dereference, so the pre-fix binary ran to completion and
// simply answered the §7.9 default.
//
// PRE-FIX (measured): exit 0, "Tag=0 Count=7"   <-- Tag should be 42
// POST-FIX:           exit 0, "Tag=42 Count=7"
//
// This is the row that makes a compile-success oracle useless: both sides of
// the fix produce a running binary with exit 0. Only the VALUE separates them.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t2_imported_int_autoprop_after_field_reads_stored_value_9_2) {
    RunResult4650 r = runImported4650(
        libSource4650(kDeclsInt4650, "I"),
        appSource4650(kBodyInt4650, "I"), "t2");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "Tag=42 Count=7"));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2 (differential control).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t2b_same_module_int_autoprop_after_field_reads_stored_value_9_2) {
    RunResult4650 r = runSingleModule4650(
        sameModule4650(kDeclsInt4650, kBodyInt4650, "I"), "t2b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "Tag=42 Count=7"));
}

// ===========================================================================
// T3 — THE SIGSEGV PATH the issue was filed on: §8.3 auto-equality over an
// `equality skip` sibling field, so `=` compares `Label` content alone.
//
// PRE-FIX (measured): both `Label`s were the zero-filled null pointer, so
//   "ab=True"  <-- WRONG: "x" and "y" are different labels
//   "ac=True"
//   then SIGSEGV inside GetHashCode, which dereferences the null string.
// POST-FIX: exit 0, "ab=False", "ac=True", "hac=True".
//
// `ab=False` is the value-differentiating assertion — it is the one that
// fails on the pre-fix compiler BEFORE the crash is reached, so this row
// does not rest on the crash alone.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t3_imported_equality_skip_content_compare_uses_stored_label_8_3) {
    RunResult4650 r = runImported4650(
        libSource4650(kDeclsEqSkip4650, "E"),
        appSource4650(kBodyEqSkip4650, "E"), "t3");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "ab=False"));
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "ac=True"));
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "hac=True"));
}

// ===========================================================================
// T3b — the SAME-MODULE twin of T3 (differential control).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t3b_same_module_equality_skip_content_compare_8_3) {
    RunResult4650 r = runSingleModule4650(
        sameModule4650(kDeclsEqSkip4650, kBodyEqSkip4650, "E"), "t3b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "ab=False"));
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "ac=True"));
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "hac=True"));
}

// ===========================================================================
// T4 — THE OFF-BY-N ROW. Two explicit fields precede two auto-properties, so
// the pre-fix desync is TWO slots, not one; and BOTH property stores were
// dropped (the pre-fix build emitted the #4045 note twice).
//
// PRE-FIX (measured): two #4045 notes, then SIGSEGV on the `Q` concatenation.
// POST-FIX: exit 0, "P=30 Q=[qq] Sum=33".
//
// A fix that advanced the index by a single slot, or that special-cased "one
// preceding field", would pass T1–T3 and fail here.
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t4_imported_two_fields_before_two_autoprops_all_stores_land_9_2) {
    RunResult4650 r = runImported4650(
        libSource4650(kDeclsMulti4650, "M"),
        appSource4650(kBodyMulti4650, "M"), "t4");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "P=30 Q=[qq] Sum=33"));
}

// ===========================================================================
// T4b — the SAME-MODULE twin of T4 (differential control).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t4b_same_module_two_fields_before_two_autoprops_9_2) {
    RunResult4650 r = runSingleModule4650(
        sameModule4650(kDeclsMulti4650, kBodyMulti4650, "M"), "t4b");
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "P=30 Q=[qq] Sum=33"));
}

// ===========================================================================
// T5 — THE MASKING CONTROL. The auto-property declared BEFORE the explicit
// field paired at index 0 correctly even pre-fix, so this row passes on BOTH
// sides. It is the "what still has to work" half of the pair: the fix widened
// the sync-walk's advance condition, and this row pins that widening did not
// disturb the shape that already resolved. Declaration order must not be
// observable in the constructed value (§9.2).
// ===========================================================================
BEZEL_TEST(xmod_imported_record_autoprop_after_field_store_4650,
           t5_imported_autoprop_before_field_still_stores_9_2) {
    RunResult4650 r = runImported4650(
        libSource4650(kDeclsPropFirst4650, "P"),
        appSource4650(kBodyString4650, "P"), "t5");
    if (r.rc == -2) return;
    BEZEL_EXPECT_EQ(r.rc, 0);
    BEZEL_EXPECT_TRUE(contains4650(r.stdout_text, "Label=[x] count=1"));
}

}  // namespace bezel::module
