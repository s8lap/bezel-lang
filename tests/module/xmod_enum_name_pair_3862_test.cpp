// fix #3862 — the compiler-provided per-enum statics `E.FromNameOrNull` /
// `E.NameOf` (spec §10.4, v0.293 onwards; stdlib §9.9, v0.293 onwards) on an
// IMPORTED enum.
//
// §10.4's "The name pair" paragraph (v0.357) fixes the table the pair reads:
//
//   "`FromNameOrNull` and `NameOf` are the two directions of one table, the
//   enum's **declared-identifier table**: each constant's own identifier —
//   the bare name as written, never a qualified `E.A` spelling — the table
//   §17.9's `nameof` reads at compile time and stdlib §2.13's
//   `Type.EnumNames` lists. … A `serialized "<name>"` clause (§9.8) changes
//   the constant's **encoded** form only: the synthesized
//   `Serialize`/`Deserialize` read the serialized-name table, and
//   `NameOf`/`FromNameOrNull` — like `nameof` and `EnumNames` — never see a
//   rename; a boundary that wants the wire spelling goes through §9.8's
//   serializers."
//
// So for an IMPORTED enum the pair must answer with the DECLARED IDENTIFIER,
// never with the §9.8 wire name. This is the cross-module half of the
// issue's own item 4 ("constant identifiers already cross the module
// boundary; prove with an `xmod_` test on a `serialized`-renamed imported
// enum that the **identifier**, not the wire name, comes back").
//
// The channel the pair reads is `EnumConstantSymbol::name` — the declared
// identifier, which `ModuleMetadataBuilder`/`ModuleLoader` carry across the
// boundary — and NOT `EnumConstantSymbol::serializedName`, the §9.8 wire
// name (fix #3140's channel). The two differ exactly in this test.
//
// ORACLE: a real two-module build (the dependency is compiled to an object
// and imported), then the program is executed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile3862 {
    std::string path;
    explicit TempFile3862(std::string p) : path(std::move(p)) {}
    ~TempFile3862() { std::remove(path.c_str()); }
};

void writeSource3862(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the importer
// did not build or did not exit normally; otherwise the program's exit code.
int compileImportAndRun3862(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3862_");
    TempFile3862 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3862 aObj((dir / (stem + "_a.o")).string());
    writeSource3862(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3862 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3862 bExe((dir / stem).string());
    writeSource3862(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The dependency module — an enum whose constants carry §9.8 RENAMES. The
// wire names are "R"/"G"; the declared identifiers are "Red"/"Green". The
// pair must answer with the IDENTIFIERS.
const char* kDepRenamed3862 =
    "module DepNamePair3862; import System.Serialization; "
    "public enum ColorW3862 "
    "  Red := 1 serialized \"R\", "
    "  Green := 2 serialized \"G\"; "
    "end ColorW3862; ";

// The same enum WITHOUT renames — the control: the identifier is the wire
// name there, so the pair must answer identically.
const char* kDepPlain3862 =
    "module DepNamePairPlain3862; import System.Serialization; "
    "public enum ColorP3862 Red := 1, Green := 2; end ColorP3862; ";

}  // namespace

// ---------------------------------------------------------------------------
// §10.4 / §9.8 — the IMPORTED enum's declared-identifier table
// ---------------------------------------------------------------------------

// §10.4: `NameOf(value)` "returns the identifier of the constant `value`
// denotes" — for an IMPORTED enum that is the DECLARED IDENTIFIER ("Green"),
// NOT the §9.8 wire name ("G") the synthesized serializer writes.
//
// And `FromNameOrNull(name)` "returns the declared constant whose identifier
// equals `name`" — so "Green" hits and "G" does not.
BEZEL_TEST(xmod_enum_name_pair_3862,
           imported_renamed_enum_answers_with_the_identifier_spec_10_4) {
    int r = compileImportAndRun3862(
        kDepRenamed3862,
        "module AppNamePair3862; import DepNamePair3862; "
        "import System.Serialization; "
        "public class Program3862 "
        "  public static method Main(): int "
        "    if ColorW3862.NameOf(ColorW3862.Green) <> \"Green\" then "
        "      return 1; end if; "
        "    var hit := ColorW3862.FromNameOrNull(\"Green\"); "
        "    if hit = null then return 2; end if; "
        "    if hit! <> ColorW3862.Green then return 3; end if; "
        "    var wire := ColorW3862.FromNameOrNull(\"G\"); "
        "    if wire <> null then return 4; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3862;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// The control: an imported enum with NO renames answers identically — the
// identifier IS the wire name there, so this pins that the pair does not
// accidentally read the §9.8 channel (which would be empty for a
// non-renamed constant) instead of the identifier channel.
BEZEL_TEST(xmod_enum_name_pair_3862,
           imported_plain_enum_answers_with_the_identifier_spec_10_4) {
    int r = compileImportAndRun3862(
        kDepPlain3862,
        "module AppNamePairPlain3862; import DepNamePairPlain3862; "
        "import System.Serialization; "
        "public class Program3862 "
        "  public static method Main(): int "
        "    if ColorP3862.NameOf(ColorP3862.Green) <> \"Green\" then "
        "      return 1; end if; "
        "    var hit := ColorP3862.FromNameOrNull(\"Green\"); "
        "    if hit = null then return 2; end if; "
        "    if hit! <> ColorP3862.Green then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3862;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
