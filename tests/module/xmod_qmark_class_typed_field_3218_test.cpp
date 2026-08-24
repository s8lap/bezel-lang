// Spec-derived regression test for issue #3218 — codegen: `imported?.ClassTypedField`
// silently drops the statement (exit 0, `internal:` only) — and works if a `.`-form
// read precedes it.
//
// §7.5 (null-conditional member access) / §9.1 (positional records) / §2.5 +
// §2.8.3 rule 4 (imported artifacts), v0.254.
//
// ── WHAT WAS MEASURED, on 226aca1, with real two-module builds ─────────────
//
// `imported?.ClassTypedField` compiled clean and exited 0 with NO output: the
// statement vanished — not even the line terminator (`internal: … #3079` on
// stderr, exit 0). The local twin printed the expected value. Inserting a `.`-form
// read of the same field BEFORE the `?.` made it work; the `.`-form was the
// trigger that forced the imported class's `classStruct` to run, populating
// `fieldIndex_` for the field. The `?.` lowering (CG41 / §7.5) looked up the
// field index WITHOUT retrying `classStruct`; the first access to an IMPORTED
// class failed because the lazy struct had never been built. The `.` spelling
// (`fieldAddr`) already retries `classStruct` on a missing entry — the #2474
// discipline — so the `?.` arm was the lone holdout.
//
// CAUSE. `fieldIndex_` is populated lazily by `classStruct`'s FIRST call for
// `ms->parent`; `declareVtables` runs it up-front for every LOCAL class but
// never for an imported one. The qualified `.` access retries `classStruct`
// on a miss; the `?.` arm did not.
//
// ── HOW IT IS FIXED ────────────────────────────────────────────────────────
//
// Same retry at the `?.` lookup site — one branch:
//   if (idxIt == fieldIndex_.end()) {
//       classStruct(ms->parent);
//       idxIt = fieldIndex_.find(ms);
//   }
// — followed by the same `return nullptr` guard the local twin already had.
//
// ── THE TESTS ─────────────────────────────────────────────────────────────
//
// T1 imports `HolderE` (which carries `public owned I: InnerE;`), reads `h?.I`
// on a non-null receiver, then `(i?.V ?? 99)` on the result. Pre-fix: the
// outer WriteLine prints nothing (exit 0, `internal:`). Post-fix: prints `i=5`.
// The single discriminator is the substring `i=5`, since exit codes are
// 8-bit-truncated and `0` cannot distinguish "ran" from "did nothing".
// T2 is the character-identical local twin — pins that the module boundary was
// the entire difference, and that the fix did not regress the local answer.
// T3 — ordering independence: `?.` AFTER a `.`-form of the SAME field must
//   already work (the issue's masking symptom); the fix makes `?.` work even
//   when the `.`-form does NOT precede it (T1 vs T3).
// T4 — null receiver: `hnull?.I` on a receiver whose field IS null yields a
//   `null` `InnerE?`, and the consumer's `??` picks the default. Pre-fix the
//   `??` default did NOT survive — same as #3177's second row.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L92 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3218() {
    return wtScratchDir("wt-3218");
}

struct TempFile3218 {
    std::string path;
    explicit TempFile3218(std::string p) : path(std::move(p)) {}
    ~TempFile3218() { std::remove(path.c_str()); }
};

void writeSource3218(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3218(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

std::string runCapture3218(const std::string& exe, int& code) {
    std::string runCmd = exe + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    std::string out;
    if (!p) { code = -2; return out; }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        out.append(buf, n);
    int status = pclose(p);
    if (status == -1)          code = -2;
    else if (WIFSIGNALED(status)) code = -3;
    else if (!WIFEXITED(status))  code = -1;
    else                          code = WEXITSTATUS(status);
    return out;
}

std::pair<int, std::string> compileImportRunCapture3218(
        const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtDir3218();
    std::string stem = unique_stem("bz3218_");
    TempFile3218 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3218 aObj((dir / (stem + "_a.o")).string());
    writeSource3218(aSrc.path, libSrc);
    if (!compileToObj3218(aSrc.path, aObj.path)) return {-2, ""};
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3218 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3218 bExe((dir / stem).string());
    writeSource3218(bSrc.path, appSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return {-1, ""};

    int code = 0;
    std::string out = runCapture3218(bExe.path, code);
    return {code, out};
}

// The single-module control: the same program with no module boundary.
std::pair<int, std::string> compileLocalRunCapture3218(const std::string& src) {
    auto dir = wtDir3218();
    std::string stem = unique_stem("bz3218_local_");
    TempFile3218 sFile((dir / (stem + ".bzl")).string());
    TempFile3218 eFile((dir / stem).string());
    writeSource3218(sFile.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, sFile.path, eFile.path, {})
             .success)
        return {-1, ""};
    int code = 0;
    std::string out = runCapture3218(eFile.path, code);
    return {code, out};
}

// ─── shared sources ──────────────────────────────────────────────────────────

// The dependency: an `InnerE` with an int field and a `HolderE` that owns it.
// The `using var h := new HolderE()` pattern matches the issue's repro; the
// `h?.I` form exercises the §7.5 path on a CLASS-TYPED FIELD of an IMPORTED
// class — the bug.
const char* kLib3218 =
    "module DepE3218; "
    "public class InnerE3218 "
    "  public V: int := 5; "
    "end InnerE3218; "
    "public class HolderE3218 "
    "  public owned I: InnerE3218; "
    "  public constructor() I := new InnerE3218(); end constructor; "
    "end HolderE3218; "
    "public class HolderNullE3218 "
    "  public owned I: InnerE3218?; "
    "  public constructor() end constructor; "
    "end HolderNullE3218;";

// Body VERBATIM shared by T1 / T3 / T4 (only `@HOLDER@` differs), so the sole
// difference between the imported and the local twin is the module boundary
// (and the holder class name, via the marker substitution). The discriminator
// `i=5` / `i=99` is what pins the printed value — exit codes are 8-bit
// truncated and `0` cannot distinguish "ran" from "did nothing".
const char* kBodyT1_3218 =
    "    using var h := new @HOLDER@(); "
    "    var i := h?.I; "
    "    Console.WriteLine(\"i=\" + (i?.V ?? 99).ToString()); ";

const char* kBodyT3_3218 =
    "    using var h := new @HOLDER@(); "
    "    Console.WriteLine(\"dot=\" + h.I.V.ToString()); "
    "    var i := h?.I; "
    "    Console.WriteLine(\"i=\" + (i?.V ?? 99).ToString()); ";

const char* kBodyT4_3218 =
    "    using var h := new @HOLDER@(); "
    "    var i := h?.I; "
    "    Console.WriteLine(\"i=\" + (i?.V ?? 99).ToString()); ";

std::string subst3218(const std::string& in, const std::string& marker,
                      const std::string& with) {
    std::string out = in;
    for (std::string::size_type i = out.find(marker);
         i != std::string::npos; i = out.find(marker, i + with.size()))
        out.replace(i, marker.size(), with);
    return out;
}

std::string localTwin3218(const char* body, const std::string& holder) {
    return std::string(
        "module LocalE3218; "
        "public class InnerE3218L "
        "  public V: int := 5; "
        "end InnerE3218L; "
        "public class HolderE3218L "
        "  public owned I: InnerE3218L; "
        "  public constructor() I := new InnerE3218L(); end constructor; "
        "end HolderE3218L; "
        "public class HolderNullE3218L "
        "  public owned I: InnerE3218L?; "
        "  public constructor() end constructor; "
        "end HolderNullE3218L; "
        "public class CE3218L "
        "  public static method Main(): int ") +
        subst3218(body, "@HOLDER@", holder) +
        "    return 0; "
        "  end Main; "
        "end CE3218L;";
}

std::string importedApp3218(const char* body, const std::string& holder) {
    return std::string(
        "module AppE3218; import DepE3218; "
        "public class CE3218 "
        "  public static method Main(): int ") +
        subst3218(body, "@HOLDER@", holder) +
        "    return 0; "
        "  end Main; "
        "end CE3218;";
}

}  // namespace

// ===========================================================================
// ★ T1 — FAILS WITHOUT THE FIX. `imported?.ClassTypedField` on a non-null
// receiver. Pre-fix stdout was empty (exit 0, `internal:` on stderr); post-fix
// stdout is "i=5\n". // per §7.5 / §2.5 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_qmark_class_typed_field_3218,
           imported_class_typed_field_through_qmark_emits_value_7_5) {
    auto [code, out] = compileImportRunCapture3218(
        kLib3218, importedApp3218(kBodyT1_3218, "HolderE3218"));
    BEZEL_EXPECT_TRUE(code != -2);  // anti-vacuity: the dependency compiled
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("i=5\n"));
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL. The character-identical program with both
// types declared in the SAME module. It was already correct before the fix
// and must stay correct: it is what pins that the module boundary was the
// entire difference. // per §7.5 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_qmark_class_typed_field_3218,
           local_twin_qmark_on_class_field_unchanged_control_7_5) {
    auto [code, out] = compileLocalRunCapture3218(
        localTwin3218(kBodyT1_3218, "HolderE3218L"));
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("i=5\n"));
}

// ===========================================================================
// ★ T3 — ORDERING INDEPENDENCE. `?.` AFTER a `.`-form of the SAME field.
// Pre-fix this WORKED — the `.`-form had already populated `fieldIndex_` for
// `I`. The fix makes T1 (the bare form) work too, so both spellings now
// produce the same output. // per §7.5 / §2.5 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_qmark_class_typed_field_3218,
           imported_dot_then_qmark_class_field_still_emits_value_7_5) {
    auto [code, out] = compileImportRunCapture3218(
        kLib3218, importedApp3218(kBodyT3_3218, "HolderE3218"));
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("dot=5\ni=5\n"));
}

// ===========================================================================
// ★ T4 — NULL RECEIVER. `h?.I` on a receiver whose class-typed field IS null
// yields `null`, and the `??` default (99) must be picked. Pre-fix this
// printed nothing — same shape as #3177's second row ("not even the default
// survived"). Post-fix prints "i=99". // per §7.5 / §2.5
// ===========================================================================
BEZEL_TEST(xmod_qmark_class_typed_field_3218,
           imported_qmark_null_field_default_survives_7_5) {
    auto [code, out] = compileImportRunCapture3218(
        kLib3218, importedApp3218(kBodyT4_3218, "HolderNullE3218"));
    BEZEL_EXPECT_TRUE(code != -2);
    if (code == -2) return;
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("i=99\n"));
}

}  // namespace bezel::module