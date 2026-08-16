// fix #2329 (§26.5 rule 4, v0.236): extension-block metadata must carry the
// nameless declaration's source site and declared `where` head so an ambiguity
// diagnostic can identify imported candidates as completely as local ones.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile2329 {
    std::string path;
    explicit TempFile2329(std::string p) : path(std::move(p)) {}
    ~TempFile2329() { std::remove(path.c_str()); }
};

void write2329(const std::string& path, const std::string& source) {
    std::ofstream out(path);
    out << source;
}

bool compileImportCapture2329(const std::string& appSource,
                              const std::string& executable,
                              const std::vector<std::string>& objects,
                              std::string& errorText) {
    namespace fs = std::filesystem;
    const std::string errorPath =
        (fs::temp_directory_path() /
         ("bz2329_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    const int saved = ::dup(STDERR_FILENO);
    const int fd = ::open(errorPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);

    std::vector<bezel::Dependency> dependencies;
    for (const std::string& object : objects)
        dependencies.push_back({object, object});
    bezel::Driver driver;
    const bool success =
        driver.compileFile(appSource, executable, dependencies).success;

    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errorPath);
    errorText.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    std::remove(errorPath.c_str());
    return success;
}

bezel::module::MetaExtension metaExtension2329(
    std::string whereClause, std::string sourceFile, std::uint32_t line) {
    using namespace bezel::module;
    MetaExtension extension;
    extension.target.kind = TypeKind::Primitive;
    extension.target.primitive = PrimitiveKind::Int;
    extension.visibility = MetaVisibility::Public;
    extension.sourceSpan.file = std::move(sourceFile);
    extension.sourceSpan.beginLine = line;
    extension.sourceSpan.beginColumn = 8;
    extension.sourceSpan.endLine = line + 2;
    extension.sourceSpan.endColumn = 5;
    if (!whereClause.empty())
        extension.genericConstraints.push_back(std::move(whereClause));
    MetaMember method;
    method.kind = MetaMemberKind::Method;
    method.name = "Pick";
    method.visibility = MetaVisibility::Public;
    method.type.kind = TypeKind::Primitive;
    method.type.primitive = PrimitiveKind::Int;
    extension.members.push_back(std::move(method));
    return extension;
}

std::string ambiguityFromMetadata2329(
    bezel::module::ModuleMetadata first,
    bezel::module::ModuleMetadata second) {
    bezel::module::MetaTypeDecl anchorA;
    anchorA.name = "AnchorA2329";
    anchorA.visibility = bezel::module::MetaVisibility::Public;
    first.types.push_back(std::move(anchorA));
    bezel::module::MetaTypeDecl anchorB;
    anchorB.name = "AnchorB2329";
    anchorB.visibility = bezel::module::MetaVisibility::Public;
    second.types.push_back(std::move(anchorB));
    auto parsed = bezel::test::parseAndCollect(
        "module App2329; import ExtA2329; import ExtB2329; "
        "class C public static method Main(): int "
        "var x: int := 1; return x.Pick(); end Main; end C;");
    if (!parsed.sourceFile || !parsed.ctx) return {};
    // Exercise the actual wire reader before Pass 4 reconstructs each synthetic
    // ExtensionDecl; this prevents an in-memory-only field from satisfying the test.
    auto firstRoundtrip = bezel::module::deserialize(
        bezel::module::serialize(first));
    auto secondRoundtrip = bezel::module::deserialize(
        bezel::module::serialize(second));
    if (!firstRoundtrip || !secondRoundtrip) return {};
    bezel::module::loadMetadata(*firstRoundtrip, *parsed.ctx);
    bezel::module::loadMetadata(*secondRoundtrip, *parsed.ctx);
    if (!parsed.ctx->importedExtensionsOf("ExtA2329")) return "missing ExtA registry";
    if (!parsed.ctx->importedExtensionsOf("ExtB2329")) return "missing ExtB registry";
    bezel::sema::resolveTypes(*parsed.sourceFile, *parsed.ctx);
    bezel::sema::typecheckExpressions(*parsed.sourceFile, *parsed.ctx);
    for (const auto& diagnostic : parsed.diagnostics.diagnostics())
        if (diagnostic.message.find("ambiguous extension member") !=
            std::string::npos)
            return diagnostic.message;
    return {};
}

}  // namespace

// §26.5 rule 4: the wire format itself preserves both diagnostic facts.
BEZEL_TEST(extension_metadata_2329,
           roundtrip_carries_source_span_and_where_clause_26_5) {
    using namespace bezel::module;
    ModuleMetadata metadata;
    metadata.moduleName = "ExtA2329";
    metadata.extensions.push_back(metaExtension2329(
        "where T: IComparable<T>, notnull", "lib/ext_a_2329.bzl", 17));
    auto roundtrip = deserialize(serialize(metadata));
    BEZEL_EXPECT_TRUE(roundtrip.has_value());
    if (!roundtrip) return;
    BEZEL_EXPECT_TRUE(*roundtrip == metadata);
    BEZEL_EXPECT_EQ(roundtrip->extensions[0].sourceSpan.file,
                    std::string("lib/ext_a_2329.bzl"));
    BEZEL_EXPECT_EQ(roundtrip->extensions[0].sourceSpan.beginLine,
                    std::uint32_t{17});
    BEZEL_EXPECT_EQ(roundtrip->extensions[0].genericConstraints[0],
                    std::string("where T: IComparable<T>, notnull"));
}

// §26.5 rule 4: two real dependency artifacts with non-trivial declaration
// lines must report BOTH original file:line:column sites, not the old
// "declaration site not tracked across module imports" fallback.
BEZEL_TEST(extension_metadata_2329,
           two_cross_module_candidates_report_both_source_sites_26_5) {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path();
    const std::string stem = unique_stem("bz2329_sites_");
    TempFile2329 aSource((dir / (stem + "_a.bzl")).string());
    TempFile2329 aObject((dir / (stem + "_a.o")).string());
    TempFile2329 bSource((dir / (stem + "_b.bzl")).string());
    TempFile2329 bObject((dir / (stem + "_b.o")).string());
    TempFile2329 appSource((dir / (stem + "_app.bzl")).string());
    TempFile2329 executable((dir / stem).string());

    write2329(aSource.path,
              "module ExtA2329; public class AnchorA2329 end AnchorA2329;\n\n\n"
              "public extension int\n"
              "  public method Pick(): int => 1;\n"
              "end;\n");
    write2329(bSource.path,
              "module ExtB2329; public class AnchorB2329 end AnchorB2329;\n\n\n\n\n"
              "public extension int\n"
              "  public method Pick(): int => 2;\n"
              "end;\n");
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileObject(aSource.path, aObject.path).success);
    BEZEL_EXPECT_TRUE(driver.compileObject(bSource.path, bObject.path).success);
    auto aBytes = bezel::module::readMetadataSection(aObject.path);
    auto bBytes = bezel::module::readMetadataSection(bObject.path);
    auto aMetadata = aBytes ? bezel::module::deserialize(*aBytes) : std::nullopt;
    auto bMetadata = bBytes ? bezel::module::deserialize(*bBytes) : std::nullopt;
    BEZEL_EXPECT_TRUE(aMetadata && aMetadata->extensions.size() == 1);
    BEZEL_EXPECT_TRUE(bMetadata && bMetadata->extensions.size() == 1);
    write2329(appSource.path,
              "module App2329;\n"
              "import ExtA2329;\n"
              "import ExtB2329;\n"
              "class C public static method Main(): int\n"
              "  var x: int := 1; return x.Pick();\n"
              "end Main; end C;\n");
    std::string errors;
    BEZEL_EXPECT_FALSE(compileImportCapture2329(
        appSource.path, executable.path, {aObject.path, bObject.path}, errors));
    BEZEL_EXPECT_TRUE(errors.find(aSource.path + ":4:8") != std::string::npos);
    BEZEL_EXPECT_TRUE(errors.find(bSource.path + ":6:8") != std::string::npos);
    BEZEL_EXPECT_TRUE(errors.find(
        "declaration site not tracked across module imports") == std::string::npos);
}

// §26.5 rule 4: Pass 4 must reconstitute every serialized `where` clause on
// both synthetic imported declarations. This uses hand-built metadata because
// the current #735 export surface deliberately excludes generic extension
// bodies pending importer-side monomorphization; the metadata/loader contract
// still must be complete for that supported future producer.
BEZEL_TEST(extension_metadata_2329,
           two_cross_module_candidates_report_both_where_clauses_26_5) {
    bezel::module::ModuleMetadata first;
    first.moduleName = "ExtA2329";
    first.extensions.push_back(metaExtension2329(
        "where T: IComparable<T>", "ext_a_2329.bzl", 17));
    bezel::module::ModuleMetadata second;
    second.moduleName = "ExtB2329";
    second.extensions.push_back(metaExtension2329(
        "where U: value", "ext_b_2329.bzl", 29));
    const std::string diagnostic =
        ambiguityFromMetadata2329(std::move(first), std::move(second));
    BEZEL_EXPECT_TRUE(diagnostic.find("where T: IComparable<T>") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(diagnostic.find("where U: value") != std::string::npos);
    BEZEL_EXPECT_TRUE(diagnostic.find("ext_a_2329.bzl:17:8") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(diagnostic.find("ext_b_2329.bzl:29:8") !=
                      std::string::npos);
}

// Regression guard: local-only ambiguity diagnostics retain their parser span
// format when no imported source-file payload is present.
BEZEL_TEST(extension_metadata_2329,
           local_only_candidate_sites_remain_reported_26_5) {
    auto parsed = bezel::test::parseAndCollect(
        "module Local2329;\n\n"
        "extension int public method Pick(): int => 1; end;\n\n\n"
        "extension int public method Pick(): int => 2; end;\n"
        "class C public static method Main(): int "
        "var x: int := 1; return x.Pick(); end Main; end C;");
    BEZEL_EXPECT_TRUE(parsed.sourceFile != nullptr && parsed.ctx != nullptr);
    if (!parsed.sourceFile || !parsed.ctx) return;
    bezel::sema::resolveTypes(*parsed.sourceFile, *parsed.ctx);
    bezel::sema::typecheckExpressions(*parsed.sourceFile, *parsed.ctx);
    std::string diagnostic;
    for (const auto& item : parsed.diagnostics.diagnostics())
        if (item.message.find("ambiguous extension member") != std::string::npos)
            diagnostic = item.message;
    BEZEL_EXPECT_TRUE(diagnostic.find("declared at line 3, column 1") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(diagnostic.find("declared at line 6, column 1") !=
                      std::string::npos);
}
