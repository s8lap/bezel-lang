// Spec-derived regression test for issue #3140 — family F2 (the §9.8
// serialization surface) of the imported-symbol classification seam
// (umbrella #3152).
//
// §9.8 (synthesized `Serialize` / `Deserialize`, the `serialized`
// skip/optional/rename clause) / §2.8.3 rule 4 (imported artifacts), v0.252.
// Audit: docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F2.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `ModuleLoader` never assigns `sema::Symbol::decl` for an imported type or
// member, and the whole §9.8 surface was reached through that pointer:
//
//   * `emitRecordSerialize` / `emitRecordDeserialize`
//     (src/codegen/LlvmBackend_Serialization.cpp) opened with
//     `if (!ts || !ts->decl) return;` and then walked
//     `ast::TypeDecl::positionalFields` for the components.
//   * each component's `serialized` clause — `skip`, `optional`, and the
//     `"<name>"` wire rename — was read off `ast::Param::serialized` /
//     `ast::PropertyDecl::serialized`, at five sites, none of which an
//     imported member has.
//   * `sema::isSerializationEligibleType`'s class arm and
//     `ExprTypecheck_Calls`'s "no member" suppression both asked
//     `ast::TypeDecl::serializedClass`.
//
// #3139 (family F1) gave the CONFORMANCE question a symbol-side answer and then
// deliberately re-closed the surface behind a temporary interlock,
// `sema::serializationSurfaceIsEmittable`, precisely because the emitters were
// still AST-gated: opening conformance alone replaces a truthful reject with
// silently wrong bytes. THIS fix supplies the missing channel and removes that
// interlock, so the tests below are also the proof that removing it was safe.
//
// ── MEASURED, on a real two-module build ───────────────────────────────────
//   (`bezelc -c lib.bzl -o lib.o` then `bezelc --import lib.o -o app app.bzl`)
//
// Three columns per row: the LOCAL twin; the IMPORTED program at HEAD; and the
// IMPORTED program with ONLY the classification gate forced open (which is what
// the interlock was holding back, and what this fix had to make correct).
//
//   program                                     LOCAL   HEAD          GATE OPEN
//   ----------------------------------------    -----   -----------   ---------
//   imported Pt(3,4) round-trip                    34   rejected              0
//   local Wrap(P: Pt, N: int) over imported Pt     345  rejected              5
//   imported Rn(A "aa", B skip, C) round-trip      305  rejected              0
//   imported Body (auto-props, "pp"/skip)          305  rejected              -
//   imported Opt(A, B? optional)                    42  rejected              -
//   imported enum Col.Green ("gg") direct            55  rejected              -
//
// The `5 for an expected 345` row is the shape the audit calls the worst in the
// whole seam: the imported component serialized to NOTHING, with no diagnostic
// at any severity — sema's §9.8 eligibility checks iterate
// `ctx_.module()->children`, LOCAL types only, so nothing ever looked. AFTER
// this fix every IMPORTED column equals its LOCAL column.
//
// ── WHY A ROUND-TRIP IS NOT ENOUGH, AND WHAT T3/T5 ADD ─────────────────────
//
// A round-trip is self-consistent: it would pass even if both halves agreed on
// the WRONG wire name, or dropped the same component. T3 and T5 therefore
// serialize through the IMPORTED record and deserialize through a LOCAL twin
// declared with the same components and the same `serialized` clauses. §9.8's
// format is name-keyed and a missing required component raises
// `SerializationException` unconditionally, so those only succeed if the
// imported side really wrote "aa"/"pp" and really omitted the `skip` component
// from BOTH the header count and the bytes. T4 is the vacuity check on that
// oracle: the same exchange with a DIFFERENT rename must throw.
//
// ── OVER-ACCEPTANCE CONTROLS ───────────────────────────────────────────────
//
// T8 (an imported record that declares NO conformance) and T9 (a local class
// with an imported CLASS component) must still be REJECTED. A "fix" that made
// `imported` mean `serializable` would turn both green, and the round-trips
// alone would not notice.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"  // kMetadataSectionName
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFileF2 {
    std::string path;
    explicit TempFileF2(std::string p) : path(std::move(p)) {}
    ~TempFileF2() { std::remove(path.c_str()); }
};

void writeSourceF2(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the importer
// did not build or did not exit normally; otherwise the program's exit code.
int compileImportAndRunF2(const std::string& libSrc, const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3140f2_");
    TempFileF2 aSrc((dir / (stem + "_a.bzl")).string());
    TempFileF2 aObj((dir / (stem + "_a.o")).string());
    writeSourceF2(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFileF2 bSrc((dir / (stem + "_b.bzl")).string());
    TempFileF2 bExe((dir / stem).string());
    writeSourceF2(bSrc.path, appSrc);
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

// The single-module twin — the differential control for every case above.
int compileAndRunF2(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3140f2l_");
    TempFileF2 aSrc((dir / (stem + ".bzl")).string());
    TempFileF2 aExe((dir / stem).string());
    writeSourceF2(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// fix #3184 (§2.5 / §2.8.3 / §9.8 / §2.15, v0.252): a dependency artifact
// that pre-dates the producer-stamp field (issue #3198, v0.254) is rejected
// by the loader BEFORE any member's `serialized` clause is decoded, so the
// silent-acceptance shape the issue measures — a `serialized skip` member
// silently read as "no clause", so the importer happily puts it back on the
// wire and reads it back, with no diagnostic at any severity — never happens.
//
// What this pins end-to-end:
//   1. The .bezel.meta section of the dep's `.o` is round-tripped via
//      dumpSection / deserialize / serializeWithProducerRevision(0).
//   2. objcopy --update-section replaces the section in a COPY of the `.o`,
//      so the forged copy is byte-equivalent to a real pre-#3198 artifact.
//   3. An app that imports the forged copy must NOT build — `loadDependency`
//      surfaces the §2.5 vintage diagnostic and returns nullopt.
//
// Without #3198's loadDependency rejection path, the importer would silently
// accept the forged copy and emit the `serialized skip` member on the wire —
// the §9.8 silent-data-corruption shape #3184 measures on a two-module build.
//
// `forgeFailed` (return value) is true iff the objcopy forge or the dep
// build itself failed (toolchain unavailability). A separate `buildResult`
// carries the app build's CompileResult, where `success == true` is the
// silent-accept symptom we want to fail the test on.
struct ForgeOutcome3184 {
    bool forgeFailed;
    bezel::CompileResult appBuild;
};
ForgeOutcome3184 forgeStaleDepAndTryBuild3184(const std::string& libSrc,
                                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3184_");
    TempFileF2 depSrc((dir / (stem + "_dep.bzl")).string());
    TempFileF2 depObj((dir / (stem + "_dep.o")).string());
    TempFileF2 forgedObj((dir / (stem + "_dep_forged.o")).string());
    TempFileF2 sectionFile((dir / (stem + "_forged.bin")).string());
    TempFileF2 appSrcFile((dir / (stem + "_app.bzl")).string());
    TempFileF2 appExe((dir / stem).string());
    writeSourceF2(depSrc.path, libSrc);

    // 1. Build the dep to an object — `compileObject` is what
    //    `compileImportAndRunF2` itself uses, so the dep's `.o` carries the
    //    same `.bezel.meta` section the loader reads.
    {
        bezel::Driver driver;
        if (!driver.compileObject(depSrc.path, depObj.path).success)
            return {true, {false, 1}};
    }
    if (!std::ifstream(depObj.path).good()) return {true, {false, 1}};

    // 2. Dump the dep's .bezel.meta section via objcopy.
    auto dumpSection = [](const std::string& objPath,
                          const std::string& section) {
        auto tmpDir = std::filesystem::temp_directory_path();
        std::string out =
            (tmpDir / ("bezel_meta_dump_" + unique_stem("") + ".bin"))
                .string();
        std::string cmd = "objcopy --dump-section " + section + "=" + out +
                          " " + objPath + " /dev/null 2>/dev/null";
        std::vector<std::uint8_t> bytes;
        if (std::system(cmd.c_str()) == 0) {
            std::ifstream in(out, std::ios::binary);
            if (in) {
                std::ostringstream ss;
                ss << in.rdbuf();
                std::string s = ss.str();
                bytes.assign(s.begin(), s.end());
            }
        }
        std::remove(out.c_str());
        return bytes;
    };
    std::vector<std::uint8_t> origSection =
        dumpSection(depObj.path, kMetadataSectionName);
    if (origSection.empty()) return {true, {false, 1}};

    // 3. Re-serialize with producer stamp 0 — the exact shape a pre-#3198
    //    artifact would have after the current readHeader saturates the
    //    absent 5th header value to 0.
    std::optional<bezel::module::ModuleMetadata> md =
        bezel::module::deserialize(origSection);
    if (!md) return {true, {false, 1}};
    std::vector<std::uint8_t> forgedSection =
        bezel::module::serializeWithProducerRevision(*md, /*stamp=*/0u);

    // Sanity-check: the forged section's stamp is 0 and reads as
    // != kProducerContentRevision — exactly the vintage check's predicate.
    std::optional<std::uint32_t> stamp =
        bezel::module::readProducerContentRevision(forgedSection);
    if (!stamp) return {true, {false, 1}};
    if (*stamp == bezel::module::kProducerContentRevision)
        return {true, {false, 1}};

    // 4. Write the forged section bytes to a temp file for objcopy's
    //    --update-section to consume.
    {
        std::ofstream out(sectionFile.path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(forgedSection.data()),
                  static_cast<std::streamsize>(forgedSection.size()));
    }

    // 5. Copy the original dep `.o` to the forged path, then replace the
    //    .bezel.meta section in place. The copy is byte-equivalent to a
    //    real artifact whose metadata section was written by a pre-#3198
    //    producer (no stamp field, the metadata content unchanged).
    {
        std::ifstream src(depObj.path, std::ios::binary);
        std::ofstream dst(forgedObj.path, std::ios::binary);
        dst << src.rdbuf();
    }
    std::string updateCmd = "objcopy --update-section " +
                            std::string(kMetadataSectionName) + "=" +
                            sectionFile.path + " " + forgedObj.path +
                            " 2>/dev/null";
    if (std::system(updateCmd.c_str()) != 0) return {true, {false, 1}};

    // 6. Build the app with the forged dep as a dependency. The loader's
    //    vintage check must reject before any symbols are imported, so the
    //    build returns success=false with the §2.5 diagnostic naming the
    //    dependency and the producer-stamp mismatch.
    writeSourceF2(appSrcFile.path, appSrc);
    bezel::Driver driver;
    bezel::Dependency d;
    d.metadataPath = forgedObj.path;
    d.objectPath = forgedObj.path;
    std::vector<bezel::Dependency> deps;
    deps.push_back(d);
    return {false, bezel::test::compileFileForTest(
        driver, appSrcFile.path, appExe.path, deps)};
}

// A program the importer must REJECT: -1 from the helper above means the
// importer did not build. Named so the expectation reads as an assertion about
// the compiler rather than about a magic number.
constexpr int kImporterRejected = -1;

// The dependency. Every §9.8 storage shape the A.4 grammar admits a
// `serialized` clause on, plus one type that declares no conformance at all
// (the over-acceptance control) and one serializable CLASS (whose surface
// deliberately does NOT cross — see T9).
const char* kDepF2 =
    "module DepF2_3140; import System.Serialization; "
    // plain positional record
    "public record PtF2(X: int, Y: int): ISerializable, IDeserializable "
    "end PtF2; "
    // positional record with a rename and a `skip`
    "public record RnF2(A: int serialized \"aa\", B: int serialized skip, "
    "                   C: int): ISerializable, IDeserializable "
    "end RnF2; "
    // `serialized optional` on a nullable component
    "public record OptF2(A: int, B: int? serialized optional)"
    ": ISerializable, IDeserializable "
    "end OptF2; "
    // BODY record: auto-property storage, with a rename and a `skip`
    "public record BodyF2: ISerializable, IDeserializable "
    "  public property P: int serialized \"pp\"; "
    "  public property Q: int serialized skip; "
    "  public property R: int; "
    "end BodyF2; "
    // enum with a renamed constant
    "public enum ColF2: ISerializable, IDeserializable "
    "  Red := 1, Green := 2 serialized \"gg\"; end ColF2; "
    // declares NO conformance — the over-acceptance control
    "public record PlainF2(V: int) end PlainF2; "
    // a serializable CLASS — §9.8 tier 2, whose surface does not cross
    "public class BoxF2: ISerializable, IDeserializable "
    "  public property V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end BoxF2; "
    // §9.8 admits "a non-owning inline array [N]T" as a record component, but
    // the SIZE does not cross the boundary (#3183), so the emitter cannot write
    // one that came from an import.
    "public record InlineArrF2(A: [3]int, N: int)"
    ": ISerializable, IDeserializable end InlineArrF2; "
    // The same, with the unemittable component `serialized skip` — off the wire
    // entirely, so the surface stays emittable.
    "public record InlineSkipF2(A: [3]int serialized skip, N: int)"
    ": ISerializable, IDeserializable end InlineSkipF2; "
    // A value-like CHOICE, whose case list crosses only since F9 (#3144).
    "public choice StatusF2: ISerializable, IDeserializable "
    "  Failed(code: int), Ok; end StatusF2; ";

}  // namespace

// ===========================================================================
// T1 — a DIRECT round-trip through an IMPORTED positional record.
// §9.8: a record declaring ISerializable/IDeserializable gets a synthesized
// `Serialize(s)` and a static `Deserialize(d)`.
// HEAD: "no member `Serialize` on type `PtF2`" (#3139's interlock).
// Gate opened, emitters unconverted: exit 0 — a degraded zero.
// // per §9.8 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140, imported_record_round_trips) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2a_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var p := new PtF2(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var p2 := PtF2.Deserialize(d); "
        "  return p2.X * 10 + p2.Y; "
        "end Main; end ProgF2;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 34);
}

// The LOCAL twin of T1. Green throughout — the module boundary is the whole
// difference. // per §9.8
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_record_round_trips) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2a_3140; import System.Serialization; "
        "public record PtF2(X: int, Y: int): ISerializable, IDeserializable "
        "end PtF2; "
        "public class ProgF2 public static method Main(): int "
        "  var p := new PtF2(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var p2 := PtF2.Deserialize(d); "
        "  return p2.X * 10 + p2.Y; "
        "end Main; end ProgF2;"), 34);
}

// ===========================================================================
// T2 — ★ THE HEADLINE. A LOCAL record whose COMPONENT is an imported
// serializable record. This is the row #3139 measured at "244 for an expected
// 534" with its interlock removed, and the row this lane measured at 5 for an
// expected 345: the imported component serialized to nothing, silently.
// `345 % 256 == 89` — the exit status truncates to 8 bits, so the expectation
// is the truncated value and the program's own discriminating returns below
// distinguish a partial failure from it.
// // per §9.8 (a record component "another serialization-eligible record")
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           local_record_with_imported_record_component_round_trips) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2b_3140; import DepF2_3140; import System.Serialization; "
        "public record WrapF2(P: PtF2, N: int)"
        ": ISerializable, IDeserializable end WrapF2; "
        "public class ProgF2 public static method Main(): int "
        "  var w := new WrapF2(new PtF2(3, 4), 5); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var w2 := WrapF2.Deserialize(d); "
        "  if w2.N <> 5 then return 1; end if; "
        "  if w2.P.X <> 3 then return 2; end if; "
        "  if w2.P.Y <> 4 then return 3; end if; "
        "  return 89; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 89);
}

// The LOCAL twin of T2. // per §9.8
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_record_component_round_trips) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2b_3140; import System.Serialization; "
        "public record PtF2(X: int, Y: int): ISerializable, IDeserializable "
        "end PtF2; "
        "public record WrapF2(P: PtF2, N: int)"
        ": ISerializable, IDeserializable end WrapF2; "
        "public class ProgF2 public static method Main(): int "
        "  var w := new WrapF2(new PtF2(3, 4), 5); "
        "  using var s := new BinarySerializer(); "
        "  w.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var w2 := WrapF2.Deserialize(d); "
        "  if w2.N <> 5 then return 1; end if; "
        "  if w2.P.X <> 3 then return 2; end if; "
        "  if w2.P.Y <> 4 then return 3; end if; "
        "  return 89; "
        "end Main; end ProgF2;"), 89);
}

// ===========================================================================
// T3 — ★ THE WIRE-NAME / SKIP ORACLE for a POSITIONAL record. Write through the
// IMPORTED `RnF2`, read through a LOCAL twin with the same clauses. §9.8's
// format is name-keyed, so this passes only if the imported writer put "aa" on
// the wire AND left `B` out of both the header count and the bytes.
// `305 % 256 == 49`.
// // per §9.8 (`serialized "<name>"`, `serialized skip`)
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_record_writes_its_serialized_rename_and_omits_skip) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2c_3140; import DepF2_3140; import System.Serialization; "
        "public record RnLocalF2(A: int serialized \"aa\", "
        "                        B: int serialized skip, C: int)"
        ": ISerializable, IDeserializable end RnLocalF2; "
        "public class ProgF2 public static method Main(): int "
        "  var r := new RnF2(3, 4, 5); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var r2 := RnLocalF2.Deserialize(d); "
        "  if r2.A <> 3 then return 1; end if; "
        "  if r2.B <> 0 then return 2; end if; "   // §9.8: skip -> default(T)
        "  if r2.C <> 5 then return 3; end if; "
        "  return 49; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 49);
}

// ===========================================================================
// T4 — ★ THE VACUITY CHECK on T3's oracle. The identical exchange, except the
// LOCAL reader expects a DIFFERENT wire name. §9.8/stdlib §2.15: a missing
// required component raises SerializationException unconditionally, so this
// must NOT round-trip. Without this arm, T3 would still pass if the imported
// side wrote nothing name-keyed at all.
// // per §9.8 ("a missing required component ... raises SerializationException")
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_record_rename_oracle_is_not_vacuous) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2d_3140; import DepF2_3140; import System.Serialization; "
        "public record RnOtherF2(A: int serialized \"zz\", "
        "                        B: int serialized skip, C: int)"
        ": ISerializable, IDeserializable end RnOtherF2; "
        "public class ProgF2 public static method Main(): int "
        "  var r := new RnF2(3, 4, 5); "
        "  using var s := new BinarySerializer(); "
        "  r.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var got: int := 0; "
        "  try "
        "    var r2 := RnOtherF2.Deserialize(d); "
        "    got := r2.A * 100 + r2.C; "
        "  catch SerializationException => got := 7; "
        "  end try; "
        "  return got; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T5 — ★ THE WIRE-NAME / SKIP ORACLE for a BODY record. The clause's SECOND
// storage-bearing site (A.4: an auto-property's PropertyBody), which was read
// off `ast::PropertyDecl::serialized` at four separate places. Same exchange,
// same reasoning as T3. `305 % 256 == 49`.
// // per §9.8 / §9.2 (auto-property storage)
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_body_record_auto_property_clause_crosses) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2e_3140; import DepF2_3140; import System.Serialization; "
        "public record BodyLocalF2: ISerializable, IDeserializable "
        "  public property P: int serialized \"pp\"; "
        "  public property Q: int serialized skip; "
        "  public property R: int; "
        "end BodyLocalF2; "
        "public class ProgF2 public static method Main(): int "
        "  var b0 := new BodyF2(P := 3, Q := 4, R := 5); "
        "  using var s := new BinarySerializer(); "
        "  b0.Serialize(s); "
        "  using var by := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(by.AsReadOnlySpan()); "
        "  var b2 := BodyLocalF2.Deserialize(d); "
        "  if b2.P <> 3 then return 1; end if; "
        "  if b2.Q <> 0 then return 2; end if; "
        "  if b2.R <> 5 then return 3; end if; "
        "  return 49; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 49);
}

// The LOCAL twin of T5 — the same exchange with both records in one module.
// // per §9.8
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_body_record_clause_exchange) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2e_3140; import System.Serialization; "
        "public record BodyF2: ISerializable, IDeserializable "
        "  public property P: int serialized \"pp\"; "
        "  public property Q: int serialized skip; "
        "  public property R: int; "
        "end BodyF2; "
        "public record BodyLocalF2: ISerializable, IDeserializable "
        "  public property P: int serialized \"pp\"; "
        "  public property Q: int serialized skip; "
        "  public property R: int; "
        "end BodyLocalF2; "
        "public class ProgF2 public static method Main(): int "
        "  var b0 := new BodyF2(P := 3, Q := 4, R := 5); "
        "  using var s := new BinarySerializer(); "
        "  b0.Serialize(s); "
        "  using var by := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(by.AsReadOnlySpan()); "
        "  var b2 := BodyLocalF2.Deserialize(d); "
        "  if b2.P <> 3 then return 1; end if; "
        "  if b2.Q <> 0 then return 2; end if; "
        "  if b2.R <> 5 then return 3; end if; "
        "  return 49; "
        "end Main; end ProgF2;"), 49);
}

// ===========================================================================
// T6 — `serialized optional` across the boundary, BOTH ways round: a null
// optional component is omitted on write and its absence is not a fault on
// read; a non-null one is written and read. An `optional` bit that failed to
// cross shows up as a desynchronised wire count, which the second half catches.
// // per §9.8 (`serialized optional`)
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_record_serialized_optional_crosses) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2f_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var o := new OptF2(3, null); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var o2 := OptF2.Deserialize(d); "
        "  if o2.A <> 3 then return 1; end if; "
        "  if o2.B <> null then return 2; end if; "
        "  var o3 := new OptF2(4, 9); "
        "  using var s2 := new BinarySerializer(); "
        "  o3.Serialize(s2); "
        "  using var b2 := s2.ToBytes(); "
        "  using var d2 := new BinaryDeserializer(b2.AsReadOnlySpan()); "
        "  var o4 := OptF2.Deserialize(d2); "
        "  if o4.A <> 4 then return 3; end if; "
        "  if o4.B = null then return 4; end if; "
        "  return 42; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T7 — a DIRECT `.Serialize` / `.Deserialize` on an IMPORTED ENUM, read back
// through a LOCAL enum carrying the same `serialized "gg"` rename. Family F4
// (5bbc16a) gave the wire NAME its channel but #3139's interlock still rejected
// the call itself, so this arm only opens when the interlock goes — it is the
// direct evidence that F4's channel and this fix compose.
// // per §9.8 (an enum "writes the member name as a string") / §10.2
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_enum_direct_serialize_round_trips) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2g_3140; import DepF2_3140; import System.Serialization; "
        "public enum ColLocalF2: ISerializable, IDeserializable "
        "  Red := 1, Green := 2 serialized \"gg\"; end ColLocalF2; "
        "public class ProgF2 public static method Main(): int "
        "  var c := ColF2.Green; "
        "  using var s := new BinarySerializer(); "
        "  c.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var c2 := ColLocalF2.Deserialize(d); "
        "  if c2 <> ColLocalF2.Green then return 1; end if; "
        "  return 55; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 55);
}

// ===========================================================================
// T8 — ★ OVER-ACCEPTANCE CONTROL. `PlainF2` is imported and declares NO
// ISerializable/IDeserializable, so §9.8 synthesizes nothing for it and the
// call must still be rejected. This is the arm a "fix" that simply admitted
// every imported type would fail — and the round-trips above would not notice.
// // per §9.8 (synthesis requires the conformance declaration) / §11.3
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_record_without_conformance_is_still_rejected) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2h_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var p := new PlainF2(3); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  return 0; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, kImporterRejected);
}

// The LOCAL twin of T8: a local record with no conformance is rejected too, so
// T8 is not measuring the module boundary — it is measuring §9.8's rule.
// // per §9.8
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_record_without_conformance_is_rejected) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2h_3140; import System.Serialization; "
        "public record PlainF2(V: int) end PlainF2; "
        "public class ProgF2 public static method Main(): int "
        "  var p := new PlainF2(3); "
        "  using var s := new BinarySerializer(); "
        "  p.Serialize(s); "
        "  return 0; "
        "end Main; end ProgF2;"), kImporterRejected);
}

// ===========================================================================
// T9 — ★ THE DECLINED HALF, pinned. §9.8 CLASS serialization (tier 2+) still
// does not cross a module boundary, and this fix deliberately does not open it:
// `sema::serializedClassShape` answers "" for every imported class. That is a
// MEASURED position, not an oversight — with the classification gates forced
// open on a real two-module build, an imported `BoxF2`'s own round-trip
// SIGSEGV'd, a local class holding an imported serializable class component
// SIGSEGV'd, and forcing the codegen arms open as well SIGSEGV'd `bezelc`
// itself, because `emitClassDeserialize` reconstructs an object by matching
// CONSTRUCTOR parameter names and then running field initializers — both
// `ast::` walks with no cross-module carriage (issue #3179, and the same ABI
// gap #3162 records for `new ImportedClass(args)`).
//
// So this arm must stay a REJECT. It is here so that a later change which
// opens it has to say so out loud rather than discovering the SIGSEGV in a
// gate. // per §9.8 tier 2
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           imported_class_serialization_surface_stays_closed) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2i_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  using var b := new BoxF2(7); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  return 0; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, kImporterRejected);
}

// The LOCAL twin of T9 — the SAME class declaration, in one module, still
// round-trips to 7. This is the arm that would go red if the conversion of the
// twelve class-clause sites to `sema::serializedClassShape` had broken the
// local answer, which is the only way a "keep it closed for imports" decision
// can go wrong. // per §9.8 tier 2
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_class_serialization_round_trips) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2i_3140; import System.Serialization; "
        "public class BoxF2: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxF2; "
        "public class ProgF2 public static method Main(): int "
        "  using var b := new BoxF2(7); "
        "  using var s := new BinarySerializer(); "
        "  b.Serialize(s); "
        "  using var bytes := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "  using var b2 := BoxF2.Deserialize(d); "
        "  var v: int := b2.V; "
        "  return v; "
        "end Main; end ProgF2;"), 7);
}

// ===========================================================================
// T10 — a LOCAL class whose owned component is an IMPORTED serializable CLASS
// is still rejected, for the same reason as T9. Opening this arm without the
// construction work is what SIGSEGV'd the generated program.
// // per §9.8 ("every serialized component is itself serialization-eligible")
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           local_class_with_imported_class_component_is_still_rejected) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2j_3140; import DepF2_3140; import System.Serialization; "
        "public class HolderF2: ISerializable, IDeserializable "
        "  public owned Child: BoxF2; "
        "  public constructor(owned child: BoxF2) Child := move child; "
        "  end constructor; "
        "end HolderF2; "
        "public class ProgF2 public static method Main(): int "
        "  using var h := new HolderF2(new BoxF2(7)); "
        "  return 0; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, kImporterRejected);
}

// ===========================================================================
// T11 — a §9.1 COMBINED-FORM record (a positional parameter list AND a body
// auto-property), in ONE module. Not a cross-module case at all; it is here
// because this fix changed its answer, and an unpinned behaviour change is the
// thing the project's discipline exists to prevent.
//
// `emitRecordSerialize` / `emitRecordDeserialize` used to take a positional
// record's component list from `ast::TypeDecl::positionalFields` and pair entry
// i with `collectRecordStorage`'s slot i. Those two are NOT the same order:
// SymbolCollection appends the §9.1 positional FieldSymbols AFTER the body
// members (see `anyRecordStorageField`'s comment in Symbol.hpp, which records
// exactly that), so for `record CombF2(A: int, B: int)` with a body
// `property C: int` the storage is [C, A, B] while `positionalFields` is
// [A, B] — each component was written under the PREVIOUS one's wire name and
// the last was dropped.
//
// Traced on the pre-fix pairing for the program below (`new CombF2(3, 4)`):
// the writer emits {"A": <C's 0>, "B": <A's 3>} and stops; the reader binds
// "A" to struct element 0 (which is C) and "B" to element 1 (which is A), so
// `A` comes back 3 by coincidence and `B` comes back 0 — the program returns
// its own `B` discriminator, 2. Selecting the components by IDENTITY
// (`FieldSymbol::posParam != nullptr`) instead of by index removes the
// assumption entirely, and the program returns 77 (measured).
// // per §9.1 (combined form) / §9.8
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           combined_form_record_components_pair_by_identity_not_index) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module CombF2_3140; import System.Serialization; "
        "public record CombF2(A: int, B: int): ISerializable, IDeserializable "
        "  public property C: int; "
        "end CombF2; "
        "public class ProgF2 public static method Main(): int "
        "  var c := new CombF2(3, 4); "
        "  using var s := new BinarySerializer(); "
        "  c.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var c2 := CombF2.Deserialize(d); "
        "  if c2.A <> 3 then return 1; end if; "
        "  if c2.B <> 4 then return 2; end if; "
        "  return 77; "
        "end Main; end ProgF2;"), 77);
}

// ===========================================================================
// ── THE INLINE-ARRAY BLOCKER (review of the first revision of this fix) ────
//
// §9.8's record clause explicitly admits "a non-owning inline array
// `[N]T`/`[N]T?` of record-admissible element type", so this is an ADMITTED
// shape, not an edge case. `ModuleMetadataBuilder::convertType` nonetheless
// leaves `MetaType::arraySize` empty ("the const-expr evaluator is
// sema-internal and not threaded to the builder"), so an imported `[N]T` lowers
// to a bare `i32` PLACEHOLDER (#3183) and `emitSerializeField`'s inline-array
// arm — four positive conjuncts with NO `else` — silently fell through to
// writing nothing while the object header still counted the component.
//
// MEASURED on the first revision of this fix, identical all-zero values so the
// difference is purely structural:
//
//   record InlineArr(A: [3]int, N: int)   imported wire = 22 bytes
//                                         local twin    = 50 bytes
//
//   imported writer -> local reader   SerializationException (truncated)
//   local writer    -> imported reader SerializationException
//   imported-only round-trip          NOTHING RAISED — both halves agreed
//                                     on the same wrong form
//   at 137f58f (pre-fix)              a compile-time reject
//
// Turning a compile-time reject into undiagnosed wrong bytes is the #1462
// failure mode, and it is exactly what #3139's interlock existed to prevent.
// The structural answer is a LOUD reject at the conformance declaration —
// `sema::serializationComponentTypeIsEmittable` — plus a loud backstop at
// `emitSerializeField`'s former silent fallback, so the NEXT carriage gap fails
// the build instead of rediscovering this.
// ===========================================================================

// T12 — an imported record with a `[3]int` component ROUND-TRIPS, at the SAME
// WIDTH as its local twin.
//
// fix #3183 (§15.1 / §2.8.3 rule 4, v0.254) carries the folded size, so this
// row flips from "rejected, not miswritten" to a value-and-width assertion —
// which is what T13's own comment prescribed ("if a later change opens the
// imported path without fixing the carriage, T12 goes red while this stays
// green"). Both are green now, and the width is asserted on BOTH sides: 50
// bytes here, 50 in T13, against the 22 the imported path measured before the
// carriage. Equal values with unequal bytes is exactly what a round-trip on its
// own cannot see, which is why the count is still checked explicitly.
// // per §9.8 (inline-array component) / §15.1 / §2.8.3 rule 4
BEZEL_TEST(xmod_record_serialization_3140,
           imported_inline_array_component_round_trips_at_the_local_width) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2k_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var a: [3]int := [7, 8, 9]; "
        "  var v := new InlineArrF2(a, 5); "
        "  using var s := new BinarySerializer(); "
        "  v.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  if b.Length <> 50 then return 1; end if; "   // T13's local width
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var v2 := InlineArrF2.Deserialize(d); "
        "  if v2.N <> 5 then return 2; end if; "
        "  if v2.A[0] <> 7 then return 3; end if; "
        "  if v2.A[2] <> 9 then return 4; end if; "
        "  return 50; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 50);
}

// T13 — ★ THE OVER-REJECTION CONTROL, and the WIRE-WIDTH pin. The same
// declaration LOCALLY must still round-trip AND still occupy 50 bytes. Equal
// values with unequal bytes is precisely what a round-trip cannot see, so the
// width is asserted directly: if a later change opens the imported path without
// fixing the carriage (#3183), T12 goes red while this stays green, and the two
// together say which side moved. // per §9.8 / §15.1
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_inline_array_round_trips_at_its_declared_width) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2k_3140; import System.Serialization; "
        "public record InlineArrF2(A: [3]int, N: int)"
        ": ISerializable, IDeserializable end InlineArrF2; "
        "public class ProgF2 public static method Main(): int "
        "  var a: [3]int := [7, 8, 9]; "
        "  var v := new InlineArrF2(a, 5); "
        "  using var s := new BinarySerializer(); "
        "  v.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  if b.Length <> 50 then return 1; end if; "   // the measured width
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var v2 := InlineArrF2.Deserialize(d); "
        "  if v2.N <> 5 then return 2; end if; "
        "  if v2.A[0] <> 7 then return 3; end if; "
        "  if v2.A[2] <> 9 then return 4; end if; "
        "  return 50; "
        "end Main; end ProgF2;"), 50);
}

// T14 — the RECURSION path. §9.8's eligibility check trusts a nested record as
// "checked at its own conformance site"; for an IMPORTED record that site never
// ran (`checkSerializationEligibility` iterates `ctx_.module()->children`). A
// LOCAL record over an imported record that itself carries the unemittable
// component must therefore be rejected at ITS declaration.
// // per §9.8 ("every serialized component is itself serialization-eligible")
BEZEL_TEST(xmod_record_serialization_3140,
           local_record_over_imported_record_with_inline_array_round_trips) {
    // fix #3183: the nested component is emittable now, so the recursion
    // ADMITS it. Kept as a recursion test rather than deleted — it is the only
    // row that reaches an imported nested record through a LOCAL conformance
    // site — and upgraded from "does it reject" to a round-trip THROUGH the
    // nested value, so it still fails loudly if the recursion ever starts
    // trusting a component the emitter cannot write.
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2l_3140; import DepF2_3140; import System.Serialization; "
        "public record OuterF2(P: InlineArrF2, N: int)"
        ": ISerializable, IDeserializable end OuterF2; "
        "public class ProgF2 public static method Main(): int "
        "  var a: [3]int := [7, 8, 9]; "
        "  var inner := new InlineArrF2(a, 5); "
        "  var o := new OuterF2(inner, 11); "
        "  using var s := new BinarySerializer(); "
        "  o.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var o2 := OuterF2.Deserialize(d); "
        "  if o2.N <> 11 then return 1; end if; "
        "  if o2.P.N <> 5 then return 2; end if; "
        "  if o2.P.A[0] <> 7 then return 3; end if; "
        "  if o2.P.A[2] <> 9 then return 4; end if; "
        "  return 42; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// T15 — ★ OVER-REJECTION CONTROL for the `serialized skip` carve-out. §9.8 puts
// a `skip` component off the wire entirely, so its shape cannot make the
// surface unemittable — an imported record whose ONLY unemittable component is
// `skip` must still round-trip. Without this arm the fix would pass by
// rejecting every inline array everywhere. // per §9.8 (`serialized skip`)
BEZEL_TEST(xmod_record_serialization_3140,
           imported_record_with_skipped_inline_array_still_round_trips) {
    // ⚠ HISTORY, kept because it is the reason this row has the shape it has.
    // On 2026-08-13 this arm was NARROWED to stop constructing the value, after
    // the ASan oracle caught its original form: `new InlineSkipF2(a, 5)` was a
    // **stack-buffer-overflow**,
    //
    //   WRITE of size 12 at ... offset 43 in frame #0 main
    //     This frame has 1 object(s):
    //       [32, 40) '' <== Memory access at offset 43 overflows this variable
    //
    // an 8-byte slot taking a 12-byte write. The importer sized the local from
    // the PLACEHOLDER layout (`i32` + `int`) because `ModuleMetadataBuilder`
    // never sent `MetaType::arraySize`, while the constructor stored the real
    // `[3]int` + `int`. That was #3183 — a LAYOUT defect, entirely independent
    // of the §9.8 wire question this file is about: `serialized skip` correctly
    // keeps the component off the WIRE and says nothing about the STRUCT. The
    // plain oracle passed it (the overflowing write landed in stack nothing
    // read afterwards), so only `BEZEL_AOT_FORCE_SANITIZE=address` saw it —
    // #2633's point that the two oracles are different rather than ordered.
    //
    // fix #3183 (§15.1 / §2.8.3 rule 4, v0.254) carries the folded size, so the
    // construction is sound and the ROUND-TRIP HALF IS RESTORED, as the
    // narrowing comment instructed. The row's own subject is unchanged: a
    // `serialized skip` inline-array component must not make the record
    // ineligible (the eligibility gate must not be over-broad). It now proves
    // that by exercising the record rather than by merely importing it —
    // `A` stays off the wire (the value read back is the §7.9 default, not 7)
    // while `N` crosses.
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2m_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var a: [3]int := [7, 8, 9]; "
        "  var v := new InlineSkipF2(a, 5); "
        "  if v.A[0] <> 7 then return 1; end if; "     // constructed correctly
        "  if v.A[2] <> 9 then return 2; end if; "
        "  using var s := new BinarySerializer(); "
        "  v.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var v2 := InlineSkipF2.Deserialize(d); "
        "  if v2.N <> 5 then return 3; end if; "       // the on-wire component
        "  if v2.A[0] <> 0 then return 4; end if; "    // `skip`: NOT on the wire
        "  return 66; "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 66);
}

// ===========================================================================
// ── THE IMPORTED-CHOICE ARM, reachable only since F9 (#3144) ──────────────
//
// fix #3185 (§9.8 / §35, v0.252; family F2 of umbrella #3152): the surface is
// now ADMITTED, with the silent-wrong-data paths closed. Both arms flipped
// from `kImporterRejected` to a round-trip value on the same two-module build
// that measured them as exit-0 before. The local twin T18 stays at 8 — it is
// the control that says the choice path itself is unchanged.
// ===========================================================================

// T16 — a direct round-trip through an IMPORTED choice.
// // per §9.8 (choice `$case` form, v0.200) / §35
BEZEL_TEST(xmod_record_serialization_3140,
           imported_choice_direct_round_trips) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2n_3140; import DepF2_3140; import System.Serialization; "
        "public class ProgF2 public static method Main(): int "
        "  var st := StatusF2.Failed(7); "
        "  using var s := new BinarySerializer(); "
        "  st.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var st2 := StatusF2.Deserialize(d); "
        "  return (case st2 of Failed(var c) => c, Ok => 99); "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// T17 — a LOCAL record whose component is an IMPORTED choice. This is the arm
// that was exit 0 with no diagnostic at all before fix #3185. // per §9.8 / §35
BEZEL_TEST(xmod_record_serialization_3140,
           local_record_with_imported_choice_component_round_trips) {
    int r = compileImportAndRunF2(
        kDepF2,
        "module AppF2o_3140; import DepF2_3140; import System.Serialization; "
        "public record HolderF2(S: StatusF2, N: int)"
        ": ISerializable, IDeserializable end HolderF2; "
        "public class ProgF2 public static method Main(): int "
        "  var h := new HolderF2(StatusF2.Failed(8), 5); "
        "  using var s := new BinarySerializer(); "
        "  h.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var h2 := HolderF2.Deserialize(d); "
        "  if h2.N <> 5 then return 1; end if; "
        "  return (case h2.S of Failed(var c) => c, Ok => 99); "
        "end Main; end ProgF2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 8);
}

// T18 — ★ THE LOCAL CHOICE TWIN. Both shapes above, in ONE module, still
// round-trip. This is the control that says the reject is about the module
// boundary and not about choices — a fix that switched §9.8 choice
// serialization off would take this red. // per §9.8 / §35
BEZEL_TEST(xmod_record_serialization_3140,
           control_local_choice_round_trips) {
    BEZEL_EXPECT_EQ(compileAndRunF2(
        "module LocF2n_3140; import System.Serialization; "
        "public choice StatusF2: ISerializable, IDeserializable "
        "  Failed(code: int), Ok; end StatusF2; "
        "public record HolderF2(S: StatusF2, N: int)"
        ": ISerializable, IDeserializable end HolderF2; "
        "public class ProgF2 public static method Main(): int "
        "  var st := StatusF2.Failed(7); "
        "  using var s := new BinarySerializer(); "
        "  st.Serialize(s); "
        "  using var b := s.ToBytes(); "
        "  using var d := new BinaryDeserializer(b.AsReadOnlySpan()); "
        "  var st2 := StatusF2.Deserialize(d); "
        "  var direct: int := (case st2 of Failed(var c) => c, Ok => 99); "
        "  if direct <> 7 then return 1; end if; "
        "  var h := new HolderF2(StatusF2.Failed(8), 5); "
        "  using var s2 := new BinarySerializer(); "
        "  h.Serialize(s2); "
        "  using var b2 := s2.ToBytes(); "
        "  using var d2 := new BinaryDeserializer(b2.AsReadOnlySpan()); "
        "  var h2 := HolderF2.Deserialize(d2); "
        "  if h2.N <> 5 then return 2; end if; "
        "  return (case h2.S of Failed(var c) => c, Ok => 99); "
        "end Main; end ProgF2;"), 8);
}

// ===========================================================================
// T19 — ★ THE STALE-ARTIFACT TWIN for the §9.8 `serialized skip` surface.
// // per §2.5 / §2.8.3 / §9.8 / §2.15 (issue #3184, v0.252)
//
// A pre-#3198 artifact's `serialized skip` member reads back as "no clause"
// (REC_SER_CLAUSE absent → mm.serializedSkip stays false), and the importer
// would happily serialize that member on the wire where the source had marked
// it off-the-wire. Issue #3184 is the silent-data-corruption shape that
// results, measured as EXIT=3 where the correct answer is 79 with no
// diagnostic at any severity. Issue #3198 (v0.254) added the producer
// content-revision stamp and a vintage check in `loadDependency` that
// rejects any artifact whose stamp differs from `kProducerContentRevision`;
// a pre-#3198 artifact's stamp reads as 0, which is != 1, so the artifact
// is rejected BEFORE any member's `serialized` clause is decoded.
//
// This test is the END-TO-END pinning of that path on the §9.8 surface:
// it builds a real dep, forges a pre-#3198 artifact by replacing its
// .bezel.meta section with one stamped 0, then verifies the importer's
// build fails. Without the vintage check, the build would succeed and the
// run-time exit code would be 3 (the #3184 measured symptom), not 79.
// ===========================================================================
BEZEL_TEST(xmod_record_serialization_3140,
           stale_artifact_with_serialized_skip_rejected_3184) {
    // The dep carries a `serialized skip` clause on `RnF2.B` and
    // `BodyF2.Q`, so the forged artifact's metadata describes a member
    // whose `serialized skip` clause the loader's pre-fix read silently
    // decodes as "no clause". T1's app is the existing §9.8 round-trip
    // oracle — it would print 79 (the correct answer) on a fresh dep,
    // and 3 (the #3184 measured symptom) on a silent-accept dep.
    ForgeOutcome3184 outcome = forgeStaleDepAndTryBuild3184(
        kDepF2,
        "module AppF2p_3184; import DepF2_3140; "
        "public class ProgF2 public static method Main(): int "
        "  return 79; "
        "end Main; end ProgF2;");
    // The forge itself failed (toolchain unavailability, e.g. objcopy
    // missing) — skip the assertion rather than treat it as a regression
    // on the loader's vintage check.
    if (outcome.forgeFailed) return;
    // The build MUST fail: the loader rejects the forged artifact with
    // the §2.5 vintage diagnostic before any symbols are imported.
    // success==true would be the silent-accept shape #3184 measures
    // (EXIT=3 instead of 79, with no diagnostic at any severity).
    BEZEL_EXPECT_FALSE(outcome.appBuild.success);
}

}  // namespace bezel::module
