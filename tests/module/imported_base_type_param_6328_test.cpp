// #6328 (§12.1, §11.2, §27.8, v0.338): a property or indexer override of an
// IMPORTED generic base is compared through the BLIND `typeRefEqual`, which
// equates every pair of declaration-less Named refs — and after the artifact
// boundary a class type parameter is exactly that: a declaration-less Named
// ref whose head is the parameter's name. So `B2<P, Q>`'s abstract `V: Q` was
// "implemented" by `override property V: P` across a module boundary, while
// the identical program in one compilation unit is rejected.
//
// #6327 cannot simply drop the imported guard at every leaf: the alias marker
// genuinely does not survive import, and an imported Builtin's name may be
// home-module-qualified on the artifact side (ModuleMetadataBuilder), so
// spelling that against the use-site spelling would false-REJECT a valid
// override. The fix therefore spells only the leaves that ARE class type
// parameters of this comparison's scope and leaves every other
// declaration-less Named leaf on the blind compare. The
// imported_builtin_*_runs row is the guard on that half.
//
// Harness copied from tests/module/override_type_param_identity_6315_module_test.cpp
// (stderr capture around the importer compile is what makes a reject row
// assert the §12.1 diagnostic rather than merely "did not compile").

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

const char* kLib6328 =
    "module Lib6328;\n"
    "public abstract class B6328<K, W>\n"
    "  public constructor() end constructor;\n"
    "  public abstract property V: W;\n"
    "  public abstract property [i: K]: W;\n"
    "end B6328;\n";

// The BASE DECLARATION spells its value type through a generic alias.
const char* kLib6328Alias =
    "module Lib6328A;\n"
    "type Id<T> := T;\n"
    "public abstract class B6328A<K, W>\n"
    "  public constructor() end constructor;\n"
    "  public abstract property V: Id<W>;\n"
    "end B6328A;\n";

// An INDIRECT imported generic base: `B6328C<A>: B6328A<A, A>`, so an app
// binding `B6328C<Q>` reaches the property's `Id<W>` through one more hop.
const char* kLib6328Chain =
    "module Lib6328C;\n"
    "type Id<T> := T;\n"
    "public abstract class B6328CB<A, W>\n"
    "  public constructor() end constructor;\n"
    "  public abstract property V: Id<W>;\n"
    "end B6328CB;\n"
    "public abstract class B6328C<A>: B6328CB<A, A>\n"
    "  public constructor() end constructor;\n"
    "end B6328C;\n";

// A compiler-known builtin on the base side: its imported name may be
// home-module-qualified, so it must NOT be spell-compared.
const char* kLib6328Builtin =
    "module Lib6328B;\n"
    "public abstract class B6328B<K>\n"
    "  public constructor() end constructor;\n"
    "  public abstract property V: Object?;\n"
    "end B6328B;\n";

struct TempFile6328 {
    std::string path;
    explicit TempFile6328(std::string p) : path(std::move(p)) {}
    ~TempFile6328() { std::remove(path.c_str()); }
};

void writeSource6328(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Outcome6328 {
    bool libBuilt = false;
    bool compiled = false;
    bool ran = false;
    int exitCode = -1;
    std::string stderrText;
    std::string stdoutText;
};

Outcome6328 buildImporter6328(const std::string& appSrc,
                              const char* libSrc) {
    namespace fs = std::filesystem;
    Outcome6328 o;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6328_");
    TempFile6328 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6328 aObj((dir / (stem + "_a.o")).string());
    writeSource6328(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        o.libBuilt = driver.compileObject(aSrc.path, aObj.path).success;
    }
    if (!o.libBuilt) return o;

    TempFile6328 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6328 bExe((dir / stem).string());
    TempFile6328 errFile((dir / (stem + "_err.txt")).string());
    TempFile6328 outFile((dir / (stem + "_out.txt")).string());
    writeSource6328(bSrc.path, appSrc);

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errFile.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        o.compiled = bezel::test::compileFileForTest(driver, bSrc.path,
                                                     bExe.path, deps)
                         .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errFile.path);
        o.stderrText.assign((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    }
    if (!o.compiled) return o;

    std::string cmd = bExe.path + " > " + outFile.path;
    int status = std::system(cmd.c_str());
    if (status != -1 && WIFEXITED(status)) {
        o.ran = true;
        o.exitCode = WEXITSTATUS(status);
    }
    std::ifstream in(outFile.path);
    std::ostringstream ss;
    ss << in.rdbuf();
    o.stdoutText = ss.str();
    return o;
}

std::string app6328(const std::string& moduleName, const std::string& libName,
                    const std::string& decls, const std::string& main) {
    return "module " + moduleName + ";\nimport " + libName + ";\n" + decls +
           "public class Program\n"
           "  public static method Main(): int\n" + main +
           "  end Main;\n"
           "end Program;\n";
}

// Reject rows: assert the app did NOT compile AND that the specific §12.1
// diagnostic fired. `libBuilt` is asserted rather than returned on, so a
// toolchain/library failure cannot read as a pass.
void expectRejected6328(const std::string& appSrc, const char* libSrc,
                        const std::string& needle) {
    Outcome6328 o = buildImporter6328(appSrc, libSrc);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_FALSE(o.compiled);
    BEZEL_EXPECT_TRUE(o.stderrText.find(needle) != std::string::npos);
}

// Accept rows: the app must COMPILE, RUN, and print `want`. Nothing here
// asserts the absence of a diagnostic on a program that compiles — the exit
// code and stdout are the oracle.
void expectRanWith6328(const std::string& appSrc, const char* libSrc,
                       const std::string& want) {
    Outcome6328 o = buildImporter6328(appSrc, libSrc);
    BEZEL_EXPECT_TRUE(o.libBuilt);
    BEZEL_EXPECT_TRUE(o.compiled);
    BEZEL_EXPECT_TRUE(o.ran);
    BEZEL_EXPECT_EQ(o.exitCode, 0);
    BEZEL_EXPECT_EQ(o.stdoutText, want);
}

}  // namespace

// The imported base's value type is `W`, bound to `Q`; the override says `P`.
// Both sides are declaration-less Named refs after import, so the blind
// compare called them the same type.
BEZEL_TEST(imported_base_type_param_6328,
           imported_property_value_other_parameter_rejected_spec_12_1) {
    expectRejected6328(
        app6328("App6328a", "Lib6328",
                "public class E6328a<P, Q>: B6328<P, Q>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: P => default(P);\n"
                "end E6328a;\n",
                "    return 0;\n"),
        kLib6328,
        "override of `V` does not match the base member's property type (§12.1)");
}

// The INDEXER's value type travels the same compare.
BEZEL_TEST(imported_base_type_param_6328,
           imported_indexer_value_other_parameter_rejected_spec_12_1) {
    expectRejected6328(
        app6328("App6328b", "Lib6328",
                "public class E6328b<P, Q>: B6328<P, Q>\n"
                "  public constructor() end constructor;\n"
                "  public override property [i: P]: P => default(P);\n"
                "end E6328b;\n",
                "    return 0;\n"),
        kLib6328,
        "override of `indexer` does not match the base member's property type (§12.1)");
}

// The base DECLARATION spells its value type through an alias: `Id<W>` is `W`,
// and with `W := Q` it is `Q`, not `P`. §27.8 makes the alias a pure synonym,
// so the alias must not launder the mismatch either — across the boundary any
// more than within one unit.
BEZEL_TEST(imported_base_type_param_6328,
           imported_base_side_alias_value_other_parameter_rejected_spec_12_1_27_8) {
    expectRejected6328(
        app6328("App6328c", "Lib6328A",
                "type Id<T> := T;\n"
                "public class E6328c<P, Q>: B6328A<P, Q>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: P => default(P);\n"
                "end E6328c;\n",
                "    return 0;\n"),
        kLib6328Alias,
        "override of `V` does not match the base member's property type (§12.1)");
}

// The two class type parameters are SWAPPED. `P` is not `Q`: an alias must not
// make two genuinely different parameters compare equal.
BEZEL_TEST(imported_base_type_param_6328,
           imported_swapped_value_type_stays_distinct_spec_12_1) {
    expectRejected6328(
        app6328("App6328d", "Lib6328",
                "public class E6328d<P, Q>: B6328<Q, P>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: Q => default(Q);\n"
                "end E6328d;\n",
                "    return 0;\n"),
        kLib6328,
        "does not match the base member's property type (§12.1)");
}

// The base is reached through an IMPORTED intermediate (`B6328C<A>: B6328A<A,
// A>`), so `Id<W>` sits two hops up the chain with `W` bound twice. `P` is
// still not `Q`.
BEZEL_TEST(imported_base_type_param_6328,
           imported_indirect_base_value_other_parameter_rejected_spec_12_1) {
    expectRejected6328(
        app6328("App6328i", "Lib6328C",
                "type Id<T> := T;\n"
                "public class E6328i<P, Q>: B6328C<Q>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: P => default(P);\n"
                "end E6328i;\n",
                "    return 0;\n"),
        kLib6328Chain,
        "override of `V` does not match the base member's property type (§12.1)");
}

// ---------------------------------------------------------------------------
// Accept rows. Each COMPILES, RUNS, and READS the overridden member before
// printing, so a row cannot pass on a body that never executed the override.
// Note the receive: an imported GENERIC base-typed reference cannot read a
// property yet (`a generic type's accessors are not exported per
// instantiation` — §14.1 / §2.8.3 rule 4 / #2884), so the read goes through the
// DERIVED-typed reference. That is enough for these rows: what they guard is
// the compile-time compare, and a false REJECT fails `o.compiled`.
// ---------------------------------------------------------------------------
BEZEL_TEST(imported_base_type_param_6328,
           imported_property_value_same_parameter_runs_spec_12_1) {
    expectRanWith6328(
        app6328("App6328e", "Lib6328",
                "public class E6328e: B6328<int, int>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: int => 41;\n"
                "  public override property [i: int]: int => i + 1;\n"
                "end E6328e;\n",
                "    var e := new E6328e();\n"
                "    Console.WriteLine(e.V + 1);\n"
                "    return 0;\n"),
        kLib6328, "42\n");
}

// The base DECLARATION spells its value type through the alias: `Id<W>` with
// `W := int` IS `int`, so the bare `int` override must keep compiling and
// running (§27.8).
BEZEL_TEST(imported_base_type_param_6328,
           imported_base_side_alias_value_bare_parameter_runs_spec_27_8) {
    expectRanWith6328(
        app6328("App6328f", "Lib6328A",
                "type Id<T> := T;\n"
                "public class E6328f: B6328A<int, int>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: int => 40;\n"
                "end E6328f;\n",
                "    var e := new E6328f();\n"
                "    Console.WriteLine(e.V + 2);\n"
                "    return 0;\n"),
        kLib6328Alias, "42\n");
}

BEZEL_TEST(imported_base_type_param_6328,
           imported_indexer_value_same_parameter_runs_spec_12_1) {
    expectRanWith6328(
        app6328("App6328g", "Lib6328",
                "public class E6328g: B6328<int, int>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: int => 41;\n"
                "  public override property [i: int]: int => i + 1;\n"
                "end E6328g;\n",
                "    var e := new E6328g();\n"
                "    Console.WriteLine(e[41]);\n"
                "    return 0;\n"),
        kLib6328, "42\n");
}

// The derived side keeps a class TYPE PARAMETER: `V: Q` against the imported
// base's `W` bound to `Q`. This is the accept direction the fix could have
// broken — reading a spelling where there is nothing to read must not reject.
BEZEL_TEST(imported_base_type_param_6328,
           imported_derived_type_parameter_matches_base_runs_spec_12_1) {
    expectRanWith6328(
        app6328("App6328j", "Lib6328",
                "public class E6328j<Q>: B6328<int, Q>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: Q => default(Q);\n"
                "  public override property [i: int]: Q => default(Q);\n"
                "end E6328j;\n",
                "    var e := new E6328j<int>();\n"
                "    Console.WriteLine(e.V);\n"
                "    return 0;\n"),
        kLib6328, "0\n");
}

// A compiler-known builtin on the base side must keep compiling: its imported
// name may be home-module-qualified (ModuleMetadataBuilder writes
// `aliasResolvedHead`, or a home-qualified spelling for an alias-qualified
// builtin), so spelling it against the use-site spelling would false-REJECT.
// This is the row that keeps the fix honest about why the imported guard is
// narrowed rather than dropped.
BEZEL_TEST(imported_base_type_param_6328,
           imported_builtin_property_value_same_type_runs_spec_12_1) {
    expectRanWith6328(
        app6328("App6328h", "Lib6328B",
                "public class E6328h: B6328B<int>\n"
                "  public constructor() end constructor;\n"
                "  public override property V: Object? => null;\n"
                "end E6328h;\n",
                "    var e := new E6328h();\n"
                "    var v := e.V;\n"
                "    Console.WriteLine(42);\n"
                "    return 0;\n"),
        kLib6328Builtin, "42\n");
}

}  // namespace bezel::module
