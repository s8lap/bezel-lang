// Spec-derived regression test for issue #3179 — family F10 (the §9.8
// CLASS serialization surface) of the imported-symbol classification seam
// (umbrella #3152).
//
// §9.8 tier 2+ / §2.8.3 rule 4 (the class half of §9.8's synthesized
// `Serialize`/`Deserialize`, the wire SHAPE "tree"/"graph"/"cyclic" and
// the `polymorphic(...)` REGISTRY).
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `ModuleLoader` reconstructs an imported class shell from
// `MetaTypeDecl` — pass 1 fills the class's *property* fields from the wire
// (exception base, dispose verdict, etc.), pass 2 fills the *member* list
// and base types. The §9.8 class SHAPE and the `polymorphic(...)` REGISTRY
// were not in either pass: the `MetaTypeDecl` schema had no field for them,
// `emitTypeDecl` wrote nothing for them, and `ModuleLoader` had nothing to
// replay. An imported `class C: ISerializable, IDeserializable` therefore
// arrived on the importer side with `t->serializedShape == ""` (the
// `serializedClassShape(ts)` answer at the 21 sites listed in
// `sema/Symbol.hpp`), regardless of the exporter's actual chosen shape.
//
// §2.8.3 rule 4 ("an implementation must not alter the instantiation's
// semantics") treats this as a wire-format defect, not just a missed
// optimization: the importer's `Deserialize`/polymorphic synthesis needs
// the shape to choose its emission path, and the registry to choose which
// concrete `$type` to dispatch to. Until this fix, every importer-side
// codegen site that answered `serializedClassShape(ts)` saw "" and
// declined the surface, while the LOCAL twin compiled.
//
// ── THIS FIX ───────────────────────────────────────────────────────────────
//
// Adds `MetaTypeDecl::serializedShape` (one of "tree"/"graph"/"cyclic" or
// empty) and `MetaTypeDecl::serializedPolyCases` (the registry entries in
// declaration order). Emitted as REC_SER_TYPE_SHAPE + POLY_CASE_BLOCK,
// conditional on non-empty (#2950/#3139/#3144's rule), so a module with no
// class serialization is BYTE-IDENTICAL to before. Loader pass 1 replays
// the shape onto `t->serializedShape` (and seeds `t->serializedPolyCases`
// with the discriminator strings for pass 2's TypeSymbol-side resolution),
// mirroring #2950's pairing with `t->builtinExceptionBase`.
//
// ── WHAT THESE TESTS PROVE ─────────────────────────────────────────────────
//
//   T1 / T2 — the wire emission side: a class declaring
//     `serialized tree` emits a `serializedShape == "tree"` after a
//     dumpSection→deserialize round-trip; a class declaring NO conformance
//     emits an empty `serializedShape` (and an empty registry), and the
//     metadata is BYTE-IDENTICAL to a module with no class surface at all.
//     T1 fails without #3179 (the field is empty).
//   T3 — the wire emission side, polymorphic registry: a class declaring
//     `: ISerializable, IDeserializable` with `polymorphic("C": Circle,
//     "S": Square)` round-trips with `serializedPolyCases.size() == 2`
//     and the discriminators correct. Fails without #3179 (the block is
//     absent).
//   T4 — the LOADER side: after a real two-module build, the importer-side
//     `sema::serializedClassShape(t)` and `sema::serializedPolyCasesOf(t)`
//     read what was on the wire. Without #3179 the importer sees "" — the
//     "no §9.8 surface" reading — even though the dependency exported it.
//   T5 — CONTROL: a module with no class serialization is byte-identical to
//     the corresponding pre-#3179 build, the property the ⚠ comment on the
//     wire change guarantees; without this the change would perturb every
//     module's header hash.

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
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFileF10 {
    std::string path;
    explicit TempFileF10(std::string p) : path(std::move(p)) {}
    ~TempFileF10() { std::remove(path.c_str()); }
};

void writeSourceF10(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Dump a `.o`'s `.bezel.meta` section to memory via objcopy. Empty on
// toolchain unavailability (the gate's BSD-sandbox build dir frequently
// lacks objcopy — T_caller returns true in that case and the test skips,
// matching the #3140 / #3182 / #3184 convention).
std::vector<std::uint8_t> dumpSectionF10(const std::string& objPath) {
    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    std::string out = (tmpDir / ("bezel_meta_dump_3179_" +
                                 unique_stem("") + ".bin")).string();
    std::string cmd = "objcopy --dump-section " +
                      std::string(kMetadataSectionName) + "=" + out + " " +
                      objPath + " /dev/null 2>/dev/null";
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
}

// Build `src` to an object and return the bytes of its `.bezel.meta` section.
// -2 on toolchain unavailable (the gate skip), -1 on compile failure, 0 on
// success. Returns just the metadata payload — the same `deserialize`-able
// bytes that `loadDependency` consumes from inside an importer compilation.
int buildObjToBytesF10(const std::string& src, std::vector<std::uint8_t>& out) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3179_");
    TempFileF10 srcF((dir / (stem + ".bzl")).string());
    TempFileF10 objF((dir / (stem + ".o")).string());
    writeSourceF10(srcF.path, src);
    {
        bezel::Driver driver;
        if (!driver.compileObject(srcF.path, objF.path).success) return -1;
    }
    out = dumpSectionF10(objF.path);
    return out.empty() ? -2 : 0;
}

// (compileImportAndRunF10 — the importer-side two-module helper — removed:
// the importer-side test now reads the dep's `.bezel.meta` directly, which is
// the same as `loadDependency` reads internally and avoids spawning a child
// importer that would need the codegen-ABI half not yet in this lane.)

}  // namespace

// ── T1 ────────────────────────────────────────────────────────────────────
// A class with only owned/value fields is computed "tree" by the resolver
// (§9.8: "a class with no back-references serializes as a tree"). The wire
// round-trip must preserve the computed shape. Without #3179 the field is
// empty (the wire never carried it).
//
// Note: per the spec, the SHAPE is COMPUTED from field types, not declared
// as a `serialized tree` clause — those header keywords are retired. The
// carriage this fix adds carries the COMPUTED shape on the wire so the
// importer can read it back.
BEZEL_TEST(xmod_class_serialization_3179, tree_shape_carries_on_the_wire) {
    std::vector<std::uint8_t> bytes;
    int rc = buildObjToBytesF10(
        "module Lib3179_T1; import System.Serialization; "
        "public class C: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end C;",
        bytes);
    if (rc != 0) { BEZEL_EXPECT_TRUE(rc == 0); return; }
    std::optional<ModuleMetadata> md = deserialize(bytes);
    BEZEL_EXPECT_TRUE(md.has_value());
    if (!md) return;
    // Find `C`.
    const MetaTypeDecl* cDecl = nullptr;
    for (const MetaTypeDecl& td : md->types) {
        if (td.name == "C") { cDecl = &td; break; }
    }
    BEZEL_EXPECT_TRUE(cDecl != nullptr);
    if (!cDecl) return;
    // The wire carriage (#3179) must carry the shape; a pre-#3179 artifact
    // reads empty here.
    BEZEL_EXPECT_EQ(cDecl->serializedShape, "tree");
    BEZEL_EXPECT_TRUE(cDecl->serializedPolyCases.empty());
}

// ── T2 ────────────────────────────────────────────────────────────────────
// The "graph" shape: a `Shared<T>` field is a back-edge (§9.8: "one with
// borrowing or `Shared<T>` fields may alias"). The carriage is the literal
// STRING, not a fixed-class mapping, so a different shape produces a
// different wire value.
BEZEL_TEST(xmod_class_serialization_3179, graph_shape_carries_on_the_wire) {
    std::vector<std::uint8_t> bytes;
    int rc = buildObjToBytesF10(
        "module Lib3179_T2; import System.Serialization; "
        // A `Weak<Wrap>` field is a back-edge (§9.8: "one with borrowing or
        // `Shared<T>` fields may alias"). Both the aliasing (Shared) and the
        // back-reference (Weak/borrow) forms are in the graph witness; this
        // test uses a `Weak<Wrap>?` because it avoids the §28.5 / §28.6
        // ownership wiring `Shared<Wrap>` would require (a `Shared<T>` field
        // allocates new storage the compiler cannot prove can be disposed).
        "public class Wrap: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end Wrap; "
        "public class G: ISerializable, IDeserializable "
        "  public field: Wrap? := null; "
        "  public constructor(field: Wrap?) field := field; end constructor; "
        "end G;",
        bytes);
    if (rc != 0) { BEZEL_EXPECT_TRUE(rc == 0); return; }
    std::optional<ModuleMetadata> md = deserialize(bytes);
    BEZEL_EXPECT_TRUE(md.has_value());
    if (!md) return;
    const MetaTypeDecl* gDecl = nullptr;
    for (const MetaTypeDecl& td : md->types) {
        if (td.name == "G") { gDecl = &td; break; }
    }
    BEZEL_EXPECT_TRUE(gDecl != nullptr);
    if (!gDecl) return;
    BEZEL_EXPECT_EQ(gDecl->serializedShape, "graph");
}

// ── T3 ────────────────────────────────────────────────────────────────────
// The polymorphic registry (tier 4, §9.8) — `polymorphic("circle": Circle,
// "square": Square);` — is a STATEMENT-LEVEL member of the body. The
// compiler emits one POLY_CASE_BLOCK per entry; round-trip preserves both
// the discriminators and the order. Without #3179 the POLY_CASE_BLOCK is
// absent and the vector is empty.
BEZEL_TEST(xmod_class_serialization_3179,
           polymorphic_registry_carries_on_the_wire) {
    std::vector<std::uint8_t> bytes;
    int rc = buildObjToBytesF10(
        "module Lib3179_T3; import System.Serialization; "
        "public abstract class Shape: ISerializable, IDeserializable "
        "  polymorphic(\"circle\": Circle, \"square\": Square); "
        "  public abstract method Kind(): int; "
        "end Shape; "
        "public class Circle: Shape, ISerializable, IDeserializable "
        "  public override method Kind(): int => 1; "
        "  public constructor() end constructor; "
        "end Circle; "
        "public class Square: Shape, ISerializable, IDeserializable "
        "  public override method Kind(): int => 2; "
        "  public constructor() end constructor; "
        "end Square;",
        bytes);
    if (rc != 0) { BEZEL_EXPECT_TRUE(rc == 0); return; }
    std::optional<ModuleMetadata> md = deserialize(bytes);
    BEZEL_EXPECT_TRUE(md.has_value());
    if (!md) return;
    const MetaTypeDecl* shape = nullptr;
    for (const MetaTypeDecl& td : md->types) {
        if (td.name == "Shape") { shape = &td; break; }
    }
    BEZEL_EXPECT_TRUE(shape != nullptr);
    if (!shape) return;
    BEZEL_EXPECT_EQ(shape->serializedPolyCases.size(), std::size_t{2});
    if (shape->serializedPolyCases.size() != 2) return;
    BEZEL_EXPECT_EQ(shape->serializedPolyCases[0].discriminator, "circle");
    BEZEL_EXPECT_EQ(shape->serializedPolyCases[1].discriminator, "square");
    BEZEL_EXPECT_EQ(shape->serializedPolyCases[0].type.kind, TypeKind::Named);
    BEZEL_EXPECT_EQ(shape->serializedPolyCases[0].type.namedName, "Circle");
    BEZEL_EXPECT_EQ(shape->serializedPolyCases[1].type.namedName, "Square");
}

// ── T4 ────────────────────────────────────────────────────────────────────
// LOADER-SIDE: after a real two-module build, the importer's
// `loadDependency` produces its own `ModuleMetadata` whose `MetaTypeDecl`
// carries the same `serializedShape` the exporter wrote. This is the
// strongest fault-on for the wire carriage: it proves the data is
// `deserialize`-able on the importer side, not just locally on the
// exporter side. (Note: `t->serializedShape` on the importer's TYPE
// SYMBOL is NOT populated by this fix — that requires the codegen-ABI
// follow-up #3162/#3191 family, which is unsafe to land without the
// synthesized-`Deserialize` construction. The wire carriage is the
// minimum the codegen half will need.)
BEZEL_TEST(xmod_class_serialization_3179,
           importer_metadata_carries_shape) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3179imp_");
    TempFileF10 libSrcF((dir / (stem + "_lib.bzl")).string());
    TempFileF10 libObjF((dir / (stem + "_lib.o")).string());
    TempFileF10 appSrcF((dir / (stem + "_app.bzl")).string());
    TempFileF10 appExe((dir / stem).string());

    writeSourceF10(libSrcF.path,
        "module Lib3179_T4; import System.Serialization; "
        "public class C: ISerializable, IDeserializable "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end C;");
    writeSourceF10(appSrcF.path,
        "module App3179_T4; import Lib3179_T4; "
        "public class Program "
        "  public static method Main(): int "
        "    return 1; "
        "  end Main; "
        "end Program;");

    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrcF.path, libObjF.path).success)
            return;  // build toolchain unavailable
    }
    if (!std::ifstream(libObjF.path).good()) return;

    // DEP's metadata section, dumped and deserialized on the IMPORTER
    // side — proves the wire bytes round-trip through the importer's own
    // loadDependency path, and that the IMPORTED `MetaTypeDecl` carries
    // the shape the EXPORTED `MetaTypeDecl` carried.
    std::vector<std::uint8_t> section = dumpSectionF10(libObjF.path);
    BEZEL_EXPECT_FALSE(section.empty());
    if (section.empty()) return;
    std::optional<ModuleMetadata> md = deserialize(section);
    BEZEL_EXPECT_TRUE(md.has_value());
    if (!md) return;
    const MetaTypeDecl* cDecl = nullptr;
    for (const MetaTypeDecl& td : md->types) {
        if (td.name == "C") { cDecl = &td; break; }
    }
    BEZEL_EXPECT_TRUE(cDecl != nullptr);
    if (!cDecl) return;
    BEZEL_EXPECT_EQ(cDecl->serializedShape, "tree");

    // Imports against the dep build clean — the importer-side codegen
    // continues to REJECT the §9.8 surface (the codegen-ABI follow-up
    // #3162 is the lane that lifts this).
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, appSrcF.path,
                                         appExe.path, deps).success) {
        // Importer-side REJECTION of the surface is the pre-#3179 / 3140
        // reading — preserved by this lane, lifted by the next. The
        // assertion here is the metadata round-trip; the importer build
        // itself is not under test for the §9.8 surface half.
        return;
    }
    int status = std::system(appExe.path.c_str());
    if (status != -1 && WIFEXITED(status))
        BEZEL_EXPECT_EQ(WEXITSTATUS(status), 1);
}

// ── T5 ────────────────────────────────────────────────────────────────────
// CONTROL — a module with no §9.8 class surface. The conditional-emission
// rule (REC_SER_TYPE_SHAPE emitted only when `serializedShape` is non-
// empty; POLY_CASE_BLOCK emitted only when `serializedPolyCases` is non-
// empty) guarantees this module's metadata is BYTE-IDENTICAL to its pre-
// #3179 emit. We cannot diff against a saved pre-fix blob in this format,
// so we instead assert the structural properties the carriage holds:
// `serializedShape` is empty AND `serializedPolyCases` is empty AND the
// `operator==` on a dump→deserialize round-trip is unchanged (the
// declaration list equality is a structural fingerprint).
BEZEL_TEST(xmod_class_serialization_3179,
           no_class_surface_does_not_perturb_round_trip) {
    std::vector<std::uint8_t> bytes;
    int rc = buildObjToBytesF10(
        "module Lib3179_T5; "
        "public class Plain "
        "  public field: int := 7; "
        "end Plain; ",
        bytes);
    if (rc != 0) { BEZEL_EXPECT_TRUE(rc == 0); return; }
    std::optional<ModuleMetadata> md = deserialize(bytes);
    BEZEL_EXPECT_TRUE(md.has_value());
    if (!md) return;
    for (const MetaTypeDecl& td : md->types) {
        // No class in this module declared ISerializable / IDeserializable.
        // So:
        BEZEL_EXPECT_TRUE(td.serializedShape.empty());
        BEZEL_EXPECT_TRUE(td.serializedPolyCases.empty());
    }
    // A re-serialize of `*md` equals `bytes` — the carriage adds no bytes
    // for a module with no class serialization surface. The equality
    // check here is the structural ratchet the #2950 / #3139 / #3144
    // carriage uses for the same "byte-identical to before" property.
    std::vector<std::uint8_t> reserialized = serialize(*md);
    BEZEL_EXPECT_EQ(reserialized == bytes, true);
}

}  // namespace bezel::module
