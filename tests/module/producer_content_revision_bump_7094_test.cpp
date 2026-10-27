// Regression test for the #7094 / #7095 stack's MISSING producer-stamp bump
// (§2.5 / §2.8.2, v0.343).
//
// THE DEFECT. `src/module/ModuleMetadata.hpp`'s own rule says a commit that
// "adds a new metadata field, a new fold into the semantic hash, a new
// modifier-rendering, or changes that ABI must bump [kProducerContentRevision]
// by hand. Without the stamp, an additive content change is LOADED SILENTLY by
// a new compiler reading an old artifact: the new field's hash fold is
// missing, the new modifier's readers hit the 'absent' branch, and the
// importer makes a wrong call about ownership, layout, or disposal — exactly
// the silent-acceptance failure shape the issue measures."
//
// The #7094 / #7095 stack triggers BOTH conditions:
//   1. a NEW METADATA FIELD — `MetaGenericParam::requiresWellDefinedDefault`,
//      a 3rd scalar of GENERIC_BLOCK's REC_SCALARS;
//   2. a NEW FOLD INTO THE SEMANTIC HASH — `hashGeneric` gained
//      `hashBool(h, g.requiresWellDefinedDefault);`.
// `kProducerContentRevision` stayed at 3, so an artifact written by the
// immediately-preceding compiler carries the SAME stamp as one written by the
// stack's compiler, and the header machinery cannot tell them apart.
//
// MEASURED, end to end (the reviewer's finding, reproduced below): the tip
// compiler reading a STALE artifact compiled the importing app with rc=0 and
// NO diagnostic, and the program then SEGFAULTED (exit 139).
//
// WHAT THIS FILE PINS.
//   1. The pre-bump producer stamp is 3 — the literal, NOT
//      `kProducerContentRevision - 1` (see the note on kPreBumpStamp below).
//   2. An artifact carrying that stamp is REJECTED by the loader, with the
//      diagnostic naming the dependency and the stamp mismatch.
//   3. An importing app built against that artifact is REJECTED — it is never
//      compiled into a program that then crashes.
//   4. CONTROL: an artifact written by the CURRENT producer is still ACCEPTED
//      — the bump rejects the stale artifact, not every artifact.
//   5. CONTROL: this harness accepts a well-formed program, so "rejected" in
//      the rows above is a verdict and not an artefact of the harness.
//
// ⚠ THE TRAP THIS FILE EXISTS TO AVOID. The stamp the fix must reject is the
// one the PRE-BUMP compiler wrote: 3. Writing `kProducerContentRevision - 1`
// would compute 2 BEFORE the bump — a stamp that was ALREADY rejected — so
// the test would pass WITHOUT the fix and prove nothing. The literal 3 is
// what makes this file RED before the bump and GREEN after it.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"  // kMetadataSectionName
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

// ── THE PRE-BUMP PRODUCER STAMP ────────────────────────────────────────────
//
// §2.5 / §2.8.2 (issue #3198, v0.254): the producer's content-revision stamp
// is a 32-bit integer the producer writes into every artifact's header
// record; the loader compares it against its own stamp and rejects the
// artifact if they differ.
//
// The value the PRE-BUMP compiler wrote is 3 — `kProducerContentRevision`
// was 3 before this fix. The literal is deliberate and load-bearing:
//
//   `kProducerContentRevision - 1` would be 2 BEFORE the bump (a stamp that
//   was already rejected), so a test written that way would pass WITHOUT the
//   fix. With the literal 3 the test is RED before the bump (the tip expects
//   3, the artifact carries 3 → silently accepted → the "must be rejected"
//   assertion FAILS) and GREEN after (the tip expects 4, the artifact carries
//   3 → rejected).
constexpr std::uint32_t kPreBumpStamp = 3;

// The exporting module: a generic class whose body takes `default(T)`. §17.11
// rejects every non-defaultable substitution of T at the instantiation site,
// so `Box<string>.Get()` is rejected in the SAME module. Across the boundary
// the obligation has to be CARRIED (#7094): the exporter computes it and
// ships it in the metadata as `MetaGenericParam::requiresWellDefinedDefault`.
const char* kDepSrc =
    "module BoxLib;\n"
    "public class Box<T>\n"
    "  public static method Get(): string => $\"{default(T)}\";\n"
    "  public constructor() end constructor;\n"
    "end Box;\n";

// The importing app: `Box<string>.Get()` — `string` is not defaultable
// (§17.11), so the app must be REJECTED when the obligation is carried, and
// must NOT be accepted when it is not.
const char* kAppSrc =
    "module App;\n"
    "import BoxLib;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    Console.WriteLine($\"{Box<string>.Get()}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// A well-formed LOCAL program — the non-vacuity control for the rows that
// assert "rejected". It has no dependency and no obligation.
const char* kLocalSrc =
    "module Local;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    Console.WriteLine(\"ok\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The loader's stamp-mismatch diagnostic (ModuleLoader.cpp). The "(got 3,
// expected 4)" pair is the observable proof that the producer stamp moved.
const char* kStampMismatchText = "different metadata content revision";
const char* kStampMismatchPair = "(got 3, expected 4)";

struct ForgedDep {
    bool ok = false;              // false → the toolchain was unavailable
    std::string depObj;           // the FORGED artifact (stamp 3, no obligation)
    std::string origObj;          // the untouched artifact (current producer)
    bool obligationCarried = false;  // the ORIGINAL metadata carried §17.11
};

// Drop the §17.11 obligation everywhere it can appear: a class's own type
// parameters, a member's own type parameters, a free function's.
void clearDefaultObligation(bezel::module::ModuleMetadata& md) {
    for (auto& t : md.types) {
        for (auto& g : t.genericParams) g.requiresWellDefinedDefault = false;
        for (auto& m : t.members)
            for (auto& g : m.genericParams) g.requiresWellDefinedDefault = false;
    }
    for (auto& f : md.functions)
        for (auto& g : f.genericParams) g.requiresWellDefinedDefault = false;
}

// Build the dependency to an object, then FORGE a copy of its artifact that
// is what a PRE-#7094 producer wrote:
//
//   * the §17.11 obligation is dropped (the reader reads a missing 3rd scalar
//     as false, so clearing the flag reproduces the pre-#7094 artifact's
//     importer-visible content exactly), and
//   * the producer stamp is the PRE-BUMP value 3.
//
// The result is the STALE artifact the reviewer's finding is about: an
// artifact a NEW compiler cannot tell apart from a current one, because the
// stamp did not move.
ForgedDep forgePreBumpDep() {
    ForgedDep out;
    namespace fs = std::filesystem;
    const fs::path dir = wtScratchDir("wt-pcr7094");
    const std::string stem = unique_stem("bz7094pcr_");
    const fs::path depSrc = dir / (stem + "_dep.bzl");
    const fs::path depObj = dir / (stem + "_dep.o");
    const fs::path forgedObj = dir / (stem + "_dep_forged.o");
    const fs::path sectionFile = dir / (stem + "_forged.bin");
    const fs::path dumpFile = dir / (stem + "_dump.bin");

    { std::ofstream o(depSrc); o << kDepSrc; }

    // 1. The dependency, built to an object — `compileObject` is the
    //    dependency-producing path, so the `.o` carries the `.bezel.meta`
    //    section the loader reads.
    {
        bezel::Driver driver;
        if (!driver.compileObject(depSrc.string(), depObj.string()).success)
            return out;
    }
    if (!std::ifstream(depObj).good()) return out;

    // 2. Dump the `.bezel.meta` section out of the object.
    {
        std::string cmd = "objcopy --dump-section " +
                          std::string(bezel::module::kMetadataSectionName) +
                          "=" + dumpFile.string() + " " + depObj.string() +
                          " /dev/null 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) return out;
    }
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(dumpFile, std::ios::binary);
        if (!in) return out;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string s = ss.str();
        bytes.assign(s.begin(), s.end());
    }
    if (bytes.empty()) return out;

    // 3. Decode it, note whether the obligation was carried, then drop it.
    std::optional<bezel::module::ModuleMetadata> md =
        bezel::module::deserialize(bytes);
    if (!md) return out;
    for (const auto& t : md->types)
        for (const auto& g : t.genericParams)
            if (g.requiresWellDefinedDefault) out.obligationCarried = true;
    clearDefaultObligation(*md);

    // 4. Re-encode with the PRE-BUMP producer stamp.
    const std::vector<std::uint8_t> forged =
        bezel::module::serializeWithProducerRevision(*md, kPreBumpStamp);
    {
        std::ofstream o(sectionFile, std::ios::binary);
        o.write(reinterpret_cast<const char*>(forged.data()),
                static_cast<std::streamsize>(forged.size()));
    }

    // 5. Replace the section in a COPY of the object, so the forged artifact
    //    is a real, linkable object whose metadata section is the stale one.
    {
        std::ifstream src(depObj, std::ios::binary);
        std::ofstream dst(forgedObj, std::ios::binary);
        dst << src.rdbuf();
    }
    {
        std::string cmd = "objcopy --update-section " +
                          std::string(bezel::module::kMetadataSectionName) +
                          "=" + sectionFile.string() + " " +
                          forgedObj.string() + " 2>/dev/null";
        if (std::system(cmd.c_str()) != 0) return out;
    }

    out.ok = true;
    out.depObj = forgedObj.string();
    out.origObj = depObj.string();
    return out;
}

// The forge is process-wide: every row needs the same artifact, and each
// forge is a compile + two objcopy invocations.
const ForgedDep& theForgedDep() {
    static const ForgedDep dep = forgePreBumpDep();
    return dep;
}

// What the loader made of an artifact, and what it said about it.
struct LoadOutcome {
    bool loaded = false;
    std::string diag;
};

LoadOutcome loadArtifact(const std::string& artifactPath) {
    LoadOutcome out;
    bezel::DiagnosticEngine diags;
    bezel::sema::SemaContext ctx(diags);
    std::optional<bezel::module::ModuleMetadata> md =
        bezel::module::loadDependency(artifactPath, ctx, &out.diag);
    out.loaded = md.has_value();
    return out;
}

// What the importer made of the program, with the driver's own diagnostics
// captured so the rejection's TEXT can be asserted (not merely "it failed").
// `depObj` empty → a LOCAL program, no dependency.
struct ImportOutcome {
    bool rejected = false;      // the program did not type-check
    std::string stampDiag;      // the loader's stamp diagnostic, if any
    std::string messages;       // every diagnostic message, joined
};

ImportOutcome importSource(const std::string& src, const std::string& depObj) {
    ImportOutcome out;
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) { out.rejected = true; return out; }
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    if (!depObj.empty()) {
        std::string diag;
        if (!bezel::module::loadDependency(depObj, ctx, &diag)) {
            out.stampDiag = diag;
            diags.report(bezel::Severity::Error, bezel::SourceLocation(), diag);
        }
    }
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    for (const bezel::Diagnostic& d : diags.diagnostics()) {
        if (!out.messages.empty()) out.messages += "\n";
        out.messages += d.message;
    }
    out.rejected = diags.hasErrors();
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE STAMP ANCHOR. The pre-bump producer stamp is 3, and the current
//    producer stamp must NOT be 3 — otherwise a stamp-3 artifact is still
//    accepted, and every row below is vacuous.
//
//    RED before the bump (3 == 3), GREEN after (3 != 4).
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           pre_bump_stamp_3_is_not_the_current_producer_stamp) {
    BEZEL_EXPECT_EQ(kPreBumpStamp, 3u);
    BEZEL_EXPECT_TRUE(kPreBumpStamp != bezel::module::kProducerContentRevision);
}

// ---------------------------------------------------------------------------
// 2. THE LOADER'S VINTAGE CHECK. A stamp-3 artifact must be refused, with the
//    diagnostic naming the dependency and the stamp mismatch.
//
//    RED before the bump (the stamp matches, so the loader ACCEPTS the
//    artifact and never writes a diagnostic), GREEN after.
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           stale_stamp_3_artifact_is_rejected_by_the_loader) {
    const ForgedDep& f = theForgedDep();
    if (!f.ok) return;  // toolchain unavailable — skip
    // The forge is only meaningful if the CURRENT producer really does carry
    // the §17.11 obligation the stale artifact is missing.
    BEZEL_EXPECT_TRUE(f.obligationCarried);

    const LoadOutcome r = loadArtifact(f.depObj);
    BEZEL_EXPECT_FALSE(r.loaded);
    BEZEL_EXPECT_TRUE(r.diag.find(kStampMismatchText) != std::string::npos);
    BEZEL_EXPECT_TRUE(r.diag.find(kStampMismatchPair) != std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. THE END-TO-END SHAPE, through the real driver. The importing app built
//    against the stale artifact must NOT build. Before the bump it DID build,
//    with no diagnostic at all, and the program it produced SEGFAULTED (exit
//    139) — the silent-acceptance failure shape the stamp exists to prevent.
//
//    RED before the bump (the app builds), GREEN after.
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           importing_app_against_stale_stamp_3_artifact_does_not_build) {
    const ForgedDep& f = theForgedDep();
    if (!f.ok) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(f.obligationCarried);

    namespace fs = std::filesystem;
    const fs::path dir = wtScratchDir("wt-pcr7094");
    const std::string stem = unique_stem("bz7094pcr_app_");
    const fs::path appSrc = dir / (stem + "_app.bzl");
    const fs::path appExe = dir / (stem + "_app.exe");
    { std::ofstream o(appSrc); o << kAppSrc; }

    bezel::Driver driver;
    bezel::Dependency d;
    d.metadataPath = f.depObj;
    d.objectPath = f.depObj;
    std::vector<bezel::Dependency> deps{d};
    const bezel::CompileResult r = bezel::test::compileFileForTest(
        driver, appSrc.string(), appExe.string(), deps);
    BEZEL_EXPECT_FALSE(r.success);
}

// ---------------------------------------------------------------------------
// 4. THE SAME REJECTION, WITH ITS TEXT. The importer's rejection must be the
//    STAMP check — the diagnostic naming the dependency and the stamp
//    mismatch — and not a §17.11 verdict: the stale artifact does not carry
//    the obligation, so §17.11 cannot be what rejected this app.
//
//    RED before the bump (the app type-checks), GREEN after.
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           importing_app_is_rejected_by_the_stamp_check_not_by_17_11) {
    const ForgedDep& f = theForgedDep();
    if (!f.ok) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(f.obligationCarried);

    const ImportOutcome r = importSource(kAppSrc, f.depObj);
    BEZEL_EXPECT_TRUE(r.rejected);
    BEZEL_EXPECT_TRUE(r.stampDiag.find(kStampMismatchText) != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stampDiag.find(kStampMismatchPair) != std::string::npos);
    BEZEL_EXPECT_TRUE(r.messages.find("not defaultable") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 5. THE CONTROL. An artifact written by the CURRENT producer is still
//    ACCEPTED — the bump rejects the stale artifact, not every artifact.
//
//    GREEN before and after the bump: this row is what makes rows 2-4
//    meaningful rather than "everything is rejected now".
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           control_current_producer_artifact_is_still_accepted) {
    const ForgedDep& f = theForgedDep();
    if (!f.ok) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(f.obligationCarried);

    const LoadOutcome r = loadArtifact(f.origObj);
    BEZEL_EXPECT_TRUE(r.loaded);
    BEZEL_EXPECT_TRUE(r.diag.empty());
}

// ---------------------------------------------------------------------------
// 6. THE NON-VACUITY CONTROL. This harness ACCEPTS a well-formed local
//    program — so "rejected" in rows 3 and 4 is a verdict about the stale
//    artifact, not an artefact of the harness.
//
//    GREEN before and after the bump.
// ---------------------------------------------------------------------------
BEZEL_TEST(producer_content_revision_bump_7094,
           control_local_program_is_accepted_by_this_harness) {
    const ImportOutcome r = importSource(kLocalSrc, std::string());
    BEZEL_EXPECT_FALSE(r.rejected);
    BEZEL_EXPECT_TRUE(r.messages.empty());
}
