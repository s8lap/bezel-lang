// fix-forward #5985 / issue #6022 (spec §27.8 and §11.2, v0.338): "an alias is
// a pure synonym ... Identity, equality and method dispatch see through the
// alias" (§27.8). An interface exported from a library whose parameter is
// typed by one of the library's aliases (`type A := Array<int>`) must be
// implemented by an importer that spells the same type directly
// (`Array<int>`), and vice versa. The exporter used to write the alias's
// USE-SITE name `A` into the metadata, which the importer cannot resolve, so
// the valid implementation was rejected by the §11.2 parameter-type match.
//
// Every row is two-module (real Driver::compileObject + compileFile with a
// Dependency). Run rows assert the program's stdout; reject rows assert the
// specific §11.2 diagnostic and that the library itself built.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct Tmp6022 {
    std::string path;
    explicit Tmp6022(std::string p) : path(std::move(p)) {}
    ~Tmp6022() { std::remove(path.c_str()); }
};

void write6022(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Result6022 {
    bool libBuilt = false;
    bool appCompiled = false;
    std::string err;
    std::string out;
    int code = -1;
};

Result6022 build6022(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz6022_");
    Tmp6022 libF((dir / (id + "_lib.bzl")).string());
    Tmp6022 libO((dir / (id + "_lib.o")).string());
    Tmp6022 appF((dir / (id + "_app.bzl")).string());
    Tmp6022 exeF((dir / id).string());
    write6022(libF.path, libSrc);
    write6022(appF.path, appSrc);
    Result6022 r;
    bezel::Driver driver;
    r.libBuilt = driver.compileObject(libF.path, libO.path).success &&
                 std::ifstream(libO.path).good();
    if (!r.libBuilt) return r;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libO.path;
    d.objectPath = libO.path;
    deps.push_back(d);

    std::string errPath = (dir / (id + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    r.appCompiled =
        bezel::test::compileFileForTest(driver, appF.path, exeF.path, deps)
            .success;
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
        std::remove(errPath.c_str());
    }
    if (!r.appCompiled) return r;
    std::string cmd = exeF.path + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return r;
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    r.code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return r;
}

// The importer: implements `LibA.ISink` with `implParamType`, calls it
// through the interface with `arg`, and prints from the implementation.
std::string app6022(const std::string& implParamType, const std::string& arg) {
    return "module AppB; import LibA; "
           "public class K: ISink "
           "  public constructor() end constructor; "
           "  public method Show(xs: " + implParamType + "): int "
           "    Console.WriteLine(\"impl-\" + xs.Count.ToString()); "
           "    return xs.Count; "
           "  end Show; "
           "end K; "
           "public class C "
           "  public static method Main(): int "
           "    var s: ISink := new K(); "
           "    var n := s.Show(" + arg + "); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string lib6022(const std::string& aliasDecl, const std::string& ifaceParam) {
    return "module LibA; " + aliasDecl +
           " public interface ISink "
           "  method Show(xs: " + ifaceParam + "): int; "
           "end ISink;";
}

bool hasMismatch6022(const Result6022& r) {
    return r.err.find("does not match interface `ISink`'s member `Show`") !=
               std::string::npos &&
           r.err.find("(\xC2\xA7""11.2)") != std::string::npos;
}

}  // namespace

BEZEL_TEST(imported_alias_iface_param_6022,
           alias_in_exported_interface_implemented_with_target_runs) {
    // Interface param is the alias; importer spells the target.
    auto r = build6022(lib6022("type A := Array<int>;", "A"),
                       app6022("Array<int>", "[1, 2, 3]"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-3\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           target_spelled_on_both_sides_control_runs) {
    // Control: the library declares the alias but its interface spells the
    // target; the importer spells the target. Must keep working.
    auto r = build6022(lib6022("type A := Array<int>;", "Array<int>"),
                       app6022("Array<int>", "[4, 5]"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-2\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           importer_alias_of_target_for_exported_alias_runs) {
    // Importer declares its own alias of the same target.
    auto r = build6022(lib6022("type A := Array<int>;", "A"),
                       "module AppB; import LibA; type B := Array<int>; "
                       "public class K: ISink "
                       "  public constructor() end constructor; "
                       "  public method Show(xs: B): int "
                       "    Console.WriteLine(\"impl-\" + xs.Count.ToString()); "
                       "    return xs.Count; "
                       "  end Show; "
                       "end K; "
                       "public class C "
                       "  public static method Main(): int "
                       "    var s: ISink := new K(); "
                       "    var n := s.Show([9]); "
                       "    return 0; "
                       "  end Main; "
                       "end C;");
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-1\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           generic_alias_in_exported_interface_runs) {
    auto r = build6022(lib6022("type L<T> := Array<T>;", "L<int>"),
                       app6022("Array<int>", "[7, 8, 9, 10]"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-4\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           alias_to_other_element_type_rejected) {
    // `type A := Array<string>` exported against an `Array<int>` implementer.
    auto r = build6022(lib6022("type A := Array<string>;", "A"),
                       app6022("Array<int>", "[1]"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(hasMismatch6022(r));
}


// ---------------------------------------------------------------------------
// An exported alias of an IMPORT-ALIASED built-in (`import System.Diagnostics
// as D; type SW := D.Stopwatch;`). The exporter's `@Home.` marker for an
// alias-qualified built-in is keyed on the use-site node, so the alias head
// must still travel as the plain target name and an importer's same-named
// type must not capture it (§2.2, §27.8, §11.2, v0.338).
// ---------------------------------------------------------------------------

namespace {

const char* kLibSw6022 =
    "module LibA; import System.Diagnostics as D; type SW := D.Stopwatch; "
    "public interface ISink "
    "  method Show(w: SW): int; "
    "end ISink;";

std::string appSw6022(const std::string& prelude, const std::string& param) {
    return "module AppB; import LibA; " + prelude +
           " public class K: ISink "
           "  public constructor() end constructor; "
           "  public method Show(w: " + param + "): int "
           "    Console.WriteLine(\"impl-sw\"); "
           "    return 1; "
           "  end Show; "
           "end K; "
           "public class C "
           "  public static method Main(): int "
           "    var s: ISink := new K(); "
           "    var w := new D.Stopwatch(); "
           "    var n := s.Show(w); "
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

BEZEL_TEST(imported_alias_iface_param_6022,
           exported_alias_of_import_aliased_builtin_direct_spelling_runs) {
    auto r = build6022(kLibSw6022,
                       appSw6022("import System.Diagnostics as D;", "D.Stopwatch"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-sw\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           exported_alias_of_import_aliased_builtin_importer_alias_runs) {
    auto r = build6022(
        kLibSw6022,
        appSw6022("import System.Diagnostics as D; type SW2 := D.Stopwatch;",
                  "SW2"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-sw\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           exported_alias_of_import_aliased_builtin_not_hijacked_by_local_class) {
    // The importer declares its OWN class `Stopwatch`; the exported alias
    // still names the built-in, so `D.Stopwatch` is the witness and the
    // importer's local type is not.
    auto r = build6022(
        kLibSw6022,
        appSw6022("import System.Diagnostics as D; "
                  "public class Stopwatch public constructor() end constructor; "
                  "end Stopwatch;",
                  "D.Stopwatch"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appCompiled);
    BEZEL_EXPECT_EQ(r.out, std::string("impl-sw\n"));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           exported_alias_of_import_aliased_builtin_local_class_rejected) {
    // An importer-local class named `Stopwatch` is a different type from the
    // exported alias's target, so it is not a witness.
    auto r = build6022(
        kLibSw6022,
        appSw6022("public class Stopwatch public constructor() end constructor; "
                  "end Stopwatch;",
                  "Stopwatch"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(hasMismatch6022(r));
}

BEZEL_TEST(imported_alias_iface_param_6022,
           exported_alias_of_import_aliased_builtin_other_builtin_rejected) {
    auto r = build6022(kLibSw6022,
                       appSw6022("", "StringBuilder"));
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(hasMismatch6022(r));
}

}  // namespace bezel::module
