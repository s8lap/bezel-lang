// Spec-derived regression test for issue #3140 — family F4 (enum constants +
// serialized wire names) of the imported-symbol classification seam
// (umbrella #3152).
//
// §9.8 (an enum "writes the member name as a string") / §10.2 / §2.8.3 rule 4
// (imported artifacts), v0.252.
// Audit: docs/proposals/papers/imported-symbol-audit-2026-08-13.md, family F4.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Emitter::emitEnumSerialize` / `emitEnumDeserialize`
// (src/codegen/LlvmBackend_Serialization.cpp) both opened with
// `if (!ts || !ts->decl) return;` and then iterated the `ast::TypeDecl`'s
// `enumConstants` for each constant's WIRE NAME. `ModuleLoader` never assigns
// `Symbol::decl` for an imported type — the importing compilation has no AST
// for a dependency's types at all — so for an IMPORTED enum the writer emitted
// NO BYTES and the reader returned a constant `0` without consuming the wire
// string. No diagnostic at any severity: sema's §9.8 eligibility checks iterate
// `ctx_.module()->children`, i.e. LOCAL types only, so nothing ever looked.
//
// The constant's VALUE has crossed the boundary since #328
// (`EnumConstantSymbol::value`, mirrored into `enumValue_`). Only the wire NAME
// had no channel at all — the audit's finding, and the reason this needed a new
// `MetaTypeDecl`/`MetaMember` record rather than only a call-the-accessor edit.
//
// ── WHY THIS FAMILY IS REACHABLE INDEPENDENTLY OF #3139/#3140's F1/F2 ──────
//
// A DIRECT `someEnum.Serialize(s)` on an imported enum is gated earlier by
// `declaresConformance(ts, "ISerializable")` — family F1, which is a separate
// channel and still answers "no" for every import. What is reachable today, and
// what these tests exercise, is the COMPONENT path: a LOCAL record that
// declares `: ISerializable, IDeserializable` (its own conformance, so F1 never
// enters) with an IMPORTED enum component. `emitSerializeField` /
// `emitDeserializeField` dispatch on the component's `TypeDeclKind::Enum` and
// land in the two functions above.
//
// ── MEASURED, on 558c27c, with a real two-module build ─────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   T1  `record Holder(C: ColorE, N: int): ISerializable, IDeserializable`,
//       `ColorE` IMPORTED, round-tripped through BinarySerializer/
//       BinaryDeserializer:  exit 2 — the program's "C did not round-trip"
//       code. `N` round-tripped; the enum came back wrong.
//       The SAME declarations as ONE module: exit 0.
//   T2  the same shape with `serialized "<name>"` renames: same failure.
//
// Silent wrong DATA, which the audit names the worst-shaped failure mode of the
// whole seam.
//
// ── WHAT T2 PINS THAT T1 CANNOT ────────────────────────────────────────────
//
// A round-trip is self-consistent: it would pass even if both halves agreed on
// the WRONG string. T2 therefore serializes through an IMPORTED enum and
// deserializes through a LOCAL enum declared with the same constants and the
// same `serialized` renames. §9.8's wire format is name-keyed, so that only
// succeeds if the imported side actually put the renamed string ("G") on the
// wire — which is exactly what the new `MetaMember::enumWireName` channel
// carries. T3 runs the same exchange in the other direction.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3140 {
    std::string path;
    explicit TempFile3140(std::string p) : path(std::move(p)) {}
    ~TempFile3140() { std::remove(path.c_str()); }
};

void writeSource3140(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the importer
// did not build or did not exit normally; otherwise the program's exit code.
int compileImportAndRun3140(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3140_");
    TempFile3140 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3140 aObj((dir / (stem + "_a.o")).string());
    writeSource3140(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3140 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3140 bExe((dir / stem).string());
    writeSource3140(bSrc.path, appSrc);
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

// The single-module twin — for the differential control.
int compileAndRun3140(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3140l_");
    TempFile3140 aSrc((dir / (stem + ".bzl")).string());
    TempFile3140 aExe((dir / stem).string());
    writeSource3140(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// A plain enum (no `serialized` clause): §9.8's wire name is the DECLARED name.
const char* kDepPlain3140 =
    "module DepF4_3140; "
    "public enum ColorE3140 Red := 1, Green := 2, Blue := 3; end ColorE3140; ";

// The same enum with §9.8 RENAMES on every constant — the fact that had no
// channel across the boundary at all before this fix.
const char* kDepRenamed3140 =
    "module DepF4R_3140; "
    "public enum ColorW3140 "
    "  Red := 1 serialized \"R\", "
    "  Green := 2 serialized \"G\", "
    "  Blue := 3 serialized \"B\"; "
    "end ColorW3140; ";

}  // namespace

// ===========================================================================
// T1 — a LOCAL serializable record with an IMPORTED enum component must
// round-trip. §9.8: an enum component "writes the member name as a string".
// Pre-fix: exit 2 — nothing was written for `C` and the read returned 0.
// The `N` check runs first, so a failure that also lost `N` would report 1 and
// be distinguishable. // per §9.8 / §10.2 / §2.8.3 rule 4
// ===========================================================================
BEZEL_TEST(xmod_enum_serialization_3140,
           imported_enum_component_round_trips) {
    int r = compileImportAndRun3140(
        kDepPlain3140,
        "module AppF4_3140a; import DepF4_3140; import System.Serialization; "
        "public record Holder3140(C: ColorE3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public class Program3140 "
        "  public static method Main(): int "
        "    var src := new Holder3140(ColorE3140.Green, 7); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var back := Holder3140.Deserialize(de); "
        "    if back.N <> 7 then return 1; end if; "
        "    if back.C <> ColorE3140.Green then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3140;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — ★ THE WIRE-NAME ORACLE. Serialize through the IMPORTED enum, deserialize
// through a LOCAL enum carrying the same constants and the same §9.8 renames.
// The format is name-keyed, so this succeeds only if the imported side put the
// RENAMED string on the wire — a round-trip alone could not tell the difference.
// // per §9.8
// ===========================================================================
BEZEL_TEST(xmod_enum_serialization_3140,
           imported_enum_writes_its_serialized_rename_on_the_wire) {
    int r = compileImportAndRun3140(
        kDepRenamed3140,
        "module AppF4_3140b; import DepF4R_3140; import System.Serialization; "
        "public enum ColorL3140 "
        "  Red := 1 serialized \"R\", "
        "  Green := 2 serialized \"G\", "
        "  Blue := 3 serialized \"B\"; "
        "end ColorL3140; "
        "public record HolderI3140(C: ColorW3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public record HolderL3140(C: ColorL3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public class Program3140 "
        "  public static method Main(): int "
        "    var src := new HolderI3140(ColorW3140.Green, 7); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var back := HolderL3140.Deserialize(de); "
        "    if back.N <> 7 then return 1; end if; "
        "    if back.C <> ColorL3140.Green then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3140;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — the READ half, in the other direction: bytes written by the LOCAL enum
// are read back through the IMPORTED one. `emitEnumDeserialize` had its own
// copy of the AST gate and returned a constant 0 without consuming the wire
// string, so a fix to the writer alone would leave this red.
// // per §9.8
// ===========================================================================
BEZEL_TEST(xmod_enum_serialization_3140,
           imported_enum_reads_the_serialized_rename_from_the_wire) {
    int r = compileImportAndRun3140(
        kDepRenamed3140,
        "module AppF4_3140c; import DepF4R_3140; import System.Serialization; "
        "public enum ColorL3140 "
        "  Red := 1 serialized \"R\", "
        "  Green := 2 serialized \"G\", "
        "  Blue := 3 serialized \"B\"; "
        "end ColorL3140; "
        "public record HolderI3140(C: ColorW3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public record HolderL3140(C: ColorL3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public class Program3140 "
        "  public static method Main(): int "
        "    var src := new HolderL3140(ColorL3140.Blue, 9); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var back := HolderI3140.Deserialize(de); "
        "    if back.N <> 9 then return 1; end if; "
        "    if back.C <> ColorW3140.Blue then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3140;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// ★ THE DIFFERENTIAL CONTROL. T2's exchange with BOTH enums declared in the
// SAME module. Green before the fix — the AST cast succeeds for a local symbol
// — so it pins that the module boundary was the whole difference, and that
// reading the wire name off `EnumConstantSymbol::serializedName` did not change
// the local answer. // per §9.8
// ===========================================================================
BEZEL_TEST(xmod_enum_serialization_3140,
           local_enum_serialized_rename_exchange_control) {
    int r = compileAndRun3140(
        "module LocalF4_3140; import System.Serialization; "
        "public enum ColorW3140 "
        "  Red := 1 serialized \"R\", "
        "  Green := 2 serialized \"G\", "
        "  Blue := 3 serialized \"B\"; "
        "end ColorW3140; "
        "public enum ColorL3140 "
        "  Red := 1 serialized \"R\", "
        "  Green := 2 serialized \"G\", "
        "  Blue := 3 serialized \"B\"; "
        "end ColorL3140; "
        "public record HolderI3140(C: ColorW3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public record HolderL3140(C: ColorL3140, N: int)"
        ": ISerializable, IDeserializable; "
        "public class Program3140 "
        "  public static method Main(): int "
        "    var src := new HolderI3140(ColorW3140.Green, 7); "
        "    using var ser := new BinarySerializer(); "
        "    src.Serialize(ser); "
        "    using var bytes := ser.ToBytes(); "
        "    using var de := new BinaryDeserializer(bytes.AsReadOnlySpan()); "
        "    var back := HolderL3140.Deserialize(de); "
        "    if back.N <> 7 then return 1; end if; "
        "    if back.C <> ColorL3140.Green then return 2; end if; "
        "    return 0; "
        "  end Main; "
        "end Program3140;");
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
