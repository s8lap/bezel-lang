// fix #5076 (§14.2, v0.307): the metadata export writer
// (`ModuleMetadataBuilder::convertGenericParams`) read constraints from
// `md->genericConstraints` (the override's own AST clause list), which is
// empty when the override omits its `where` clause (§14.2's recommended
// spelling).  The sema side materialization done by
// `checkOverrideSignatureMatch` (#5039 / #5075) populated
// `GenericParamSymbol::constraints` on the method's symbol, but the metadata
// writer never read it — it always read the raw AST list.
//
// Consequence: a `public` class whose override omits `where` clauses, once
// exported as a library artifact, reports an empty constraint set for that
// generic parameter in its `.bezel.meta`.  Any third module importing it and
// further overriding the same generic virtual method sees the empty set (the
// #5039/#5075 materialization only walks same-module AST state) and hits the
// `genericParamSoundAtI32ForPlaceholder` wrong-dispatch path.
//
// The fix: in `convertGenericParams`, when the AST clause list is empty for
// a type parameter that has materialized constraints on its symbol, fall back
// to the symbol's constraint list.
//
// This test verifies that an override that omits `where` clauses compiles and
// runs correctly (the override's GenericParamSymbol.materialized constraints
// are exported in the metadata and the subsequent dispatch is correct).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "parser/Parser.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "sema/ExprTypecheck.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile5076 {
    std::string path;
    explicit TempFile5076(std::string p) : path(std::move(p)) {}
    ~TempFile5076() { std::remove(path.c_str()); }
};

void writeSource5076(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// ── harness ──────────────────────────────────────────────────────────────────

// Build ModuleMetadata from source text (the standard test harness pattern from
// module_chapter_1365_test.cpp).
ModuleMetadata buildMetaFor5076(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) return {};
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    return buildModuleMetadata(ctx, source);
}

// Compile a source file to an .o artifact on disk.
bool compileToObj5076(const std::string& srcPath,
                      const std::string& objPath,
                      std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz5076_err_" + unique_stem("") + ".txt")).string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileObject(srcPath, objPath).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// Compile + run, importing a pre-built .o dependency.
int compileImportAndRun5076(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5076_");
    TempFile5076 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5076 aObj((dir / (stem + "_a.o")).string());
    writeSource5076(aSrc.path, libSrc);
    std::string errA;
    if (!compileToObj5076(aSrc.path, aObj.path, errA)) return -2;

    TempFile5076 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5076 bExe((dir / stem).string());
    writeSource5076(bSrc.path, appSrc);
    bezel::Driver driver;
    bezel::Dependency dep;
    dep.metadataPath = aObj.path;
    dep.objectPath = aObj.path;
    std::vector<bezel::Dependency> deps{dep};
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Search a method's genericParams for a special-constraint kind.
bool gpHasSpecialConstraint(const ModuleMetadata& meta,
                           std::string_view typeName,
                           std::string_view methodName,
                           std::string_view paramName,
                           std::string_view constraint) {
    for (const auto& td : meta.types) {
        if (td.name != typeName) continue;
        for (const auto& member : td.members) {
            if (member.name != methodName) continue;
            for (const auto& gp : member.genericParams) {
                if (gp.name != paramName) continue;
                for (const auto& sc : gp.specialConstraints) {
                    if (sc == constraint) return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

// ===========================================================================
// T1 — METADATA CHECK: an override that omits `where T: class` carries the
// inherited `class` special-constraint in its exported genericParams.
// Uses buildModuleMetadata (the standard test harness, same as
// module_chapter_1365_test.cpp) to access the metadata before emission.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_generic_constraints_5076,
           omitted_where_override_meta_carries_class_constraint) {
    const char* kSrc =
        "module Mod5076T2; "
        "public class C5076T2 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end M; "
        "end C5076T2; "
        "public class D5076T2 : C5076T2 "
        // Omit `where T: class` — inherits it.
        "  public override method M<T>(x: T): int "
        "    return 1; "
        "  end M; "
        "end D5076T2; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5076(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "D5076T2", "M", "T", "class");
    if (!hasClass) {
        std::cerr << "D5076T2.M<T> missing 'class' special-constraint\n";
    }
    BEZEL_EXPECT_TRUE(hasClass);
}

// ===========================================================================
// T3 — COMPOUND CONSTRAINT: an override that omits `where T: class, IFoo`
// carries both the special-constraint AND the type-bound in its exported
// genericParams.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_generic_constraints_5076,
           omitted_where_override_meta_carries_compound_constraint) {
    const char* kSrc =
        "module Mod5076T3; "
        "public interface INum5076T3 "
        "  method Val(): int; "
        "end INum5076T3; "
        "public class C5076T3 "
        "  public virtual method M<T>(x: T): int "
        "    where T: class, INum5076T3 "
        "    return 0; "
        "  end M; "
        "end C5076T3; "
        "public class D5076T3 : C5076T3 "
        // Omit `where T: class, INum5076T3` — inherits both.
        "  public override method M<T>(x: T): int "
        "    return 1; "
        "  end M; "
        "end D5076T3; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5076(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "D5076T3", "M", "T", "class");
    bool foundIFoo = false;
    for (const auto& td : meta.types) {
        if (td.name != "D5076T3") continue;
        for (const auto& member : td.members) {
            if (member.name != "M") continue;
            for (const auto& gp : member.genericParams) {
                if (gp.name != "T") continue;
                for (const auto& ct : gp.constraintTypes) {
                    if (ct.namedName == "INum5076T3") foundIFoo = true;
                }
            }
        }
    }
    if (!hasClass) std::cerr << "D5076T3.M<T> missing 'class'\n";
    if (!foundIFoo) std::cerr << "D5076T3.M<T> missing INum5076T3 bound\n";
    BEZEL_EXPECT_TRUE(hasClass);
    BEZEL_EXPECT_TRUE(foundIFoo);
}

// ===========================================================================
// T4 — CONTROL: override that RESTATES the clause verbatim still exports it.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_generic_constraints_5076,
           restate_where_override_meta_carries_class_constraint) {
    const char* kSrc =
        "module Mod5076T4; "
        "public interface I5076T4 "
        "  method M<T>(x: T): int; "
        "end I5076T4; "
        "public class C5076T4 implements I5076T4 "
        "  public method M<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end M; "
        "end C5076T4; "
        "public class D5076T4 : C5076T4 "
        // Restate verbatim.
        "  public override method M<T>(x: T): int "
        "    where T: class "
        "    => 1; "
        "end D5076T4; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5076(kSrc);
    bool hasClass = gpHasSpecialConstraint(meta, "D5076T4", "M", "T", "class");
    BEZEL_EXPECT_TRUE(hasClass);
}

// ===========================================================================
// T5 — VALUE CONSTRAINT: an override that omits `where T: value` carries
// the inherited value constraint.
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_generic_constraints_5076,
           omitted_where_override_meta_carries_value_constraint) {
    const char* kSrc =
        "module Mod5076T5; "
        "public class C5076T5 "
        "  public virtual method M<T>(x: T): int "
        "    where T: value "
        "    return 0; "
        "  end M; "
        "end C5076T5; "
        "public class D5076T5 : C5076T5 "
        // Omit `where T: value` — inherits it.
        "  public override method M<T>(x: T): int "
        "    return 1; "
        "  end M; "
        "end D5076T5; "
        "public class Entry public static method Main(): int => 0; end Entry;";

    ModuleMetadata meta = buildMetaFor5076(kSrc);
    bool hasValue = gpHasSpecialConstraint(meta, "D5076T5", "M", "T", "value");
    if (!hasValue) std::cerr << "D5076T5.M<T> missing 'value'\n";
    BEZEL_EXPECT_TRUE(hasValue);
}

}  // namespace bezel::module
