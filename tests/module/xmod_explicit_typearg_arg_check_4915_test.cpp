// Regression tests for issue #4915 (sema half, rv-L29 review rework) — the
// IMPORTED-method twin of `checkArgsAgainstParams`'s explicit type-argument
// handling, in `checkImportedCallArity`.
//
// spec §14.5 "Type-argument inference for generic methods", v0.307: "An
// explicit type-argument list ... binds the declared type parameters in
// declaration order"; "When a full explicit list is supplied, no inference
// runs." §5.1: no implicit conversion from a primitive to a type parameter
// (or from `int` to `string`). §2.8.3 rule 2: an imported artifact must not
// alter an instantiation's semantics relative to one module.
//
// THE GAP. The imported path checked each argument against the callee's
// DECLARED formal (`x: T`) and passed the callee's parameters as the free set,
// with no explicit-list substitution (the local path has had one since
// #4325). #4915's by-name exemption of free parameters from the #4810 arm then
// admitted `LUtil.Show<T>(77)` inside a `Go<T>` (T = a class at the call), and
// `LUtil.Show<string>(77)` was admitted anywhere, since the unsubstituted `T`
// is free. MEASURED (two-module `bezelc -c` / `--import` builds): both
// compiled and ran. The fix substitutes the explicit list into the formal and
// drops the bound names from the free set, as the local path does.
//
// Rows: two reject rows assert the §5.1 diagnostic substring; two accept rows
// RUN and assert the exit code (an inferred same-name call — the #4915 accept
// — and an explicit `<int>` call inside a generic caller).

#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

const fs::path& dir4915x() { return bezel_test::wtScratchDir("wt-4915x"); }

void write4915x(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

const char* kLib4915x =
    "module Lib4915x;\n"
    "public class LUtil\n"
    "  public static method Echo<T>(x: T): T\n"
    "    return x;\n"
    "  end Echo;\n"
    "  public static method Show<T>(x: T): string\n"
    "    return $\"{x}\";\n"
    "  end Show;\n"
    "end LUtil;\n";

struct Result4915x {
    bool libOk = false;
    bool compiled = false;
    int exitCode = -1;
    std::string err;
};

// Build the library, then compile (and, when it compiles, run) the importer.
// stderr is captured around the importer's compile so the diagnostic text can
// be matched (the shape tests/module/xmod_explicit_typeargs_imported_4622
// established).
Result4915x build4915x(const std::string& appSrc) {
    Result4915x r;
    const std::string stem = unique_stem("x4915_");
    const std::string libSrc = (dir4915x() / (stem + "_lib.bzl")).string();
    const std::string libObj = (dir4915x() / (stem + "_lib.o")).string();
    const std::string appPath = (dir4915x() / (stem + "_app.bzl")).string();
    const std::string exe = (dir4915x() / stem).string();
    const std::string errPath = (dir4915x() / (stem + "_err.txt")).string();
    write4915x(libSrc, kLib4915x);
    write4915x(appPath, appSrc);
    {
        bezel::Driver driver;
        r.libOk = driver.compileObject(libSrc, libObj).success &&
                  std::ifstream(libObj).good();
    }
    if (!r.libOk) return r;
    std::fflush(stderr);
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObj;
        d.objectPath = libObj;
        deps.push_back(d);
        r.compiled =
            bezel::test::compileFileForTest(driver, appPath, exe, deps).success;
    }
    std::fflush(stderr);
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    {
        std::ifstream in(errPath);
        r.err.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    if (r.compiled) {
        int status = bezel::test::runProgramForTest(exe);
        r.exitCode =
            (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    }
    return r;
}

}  // namespace

// ═════════ Reject rows ═════════════════════════════════════════════════════

// Explicit same-name argument inside a generic caller: the formal is the
// caller's `T` (= class `A` at the call), which `int` does not convert to.
BEZEL_TEST(xmod_explicit_typearg_arg_check_4915,
           imported_explicit_same_name_type_arg_int_rejected_spec_14_5_5_1) {
    auto r = build4915x(
        "module App4915x;\n"
        "import Lib4915x;\n"
        "public class A\n"
        "  public v: int := 3;\n"
        "end A;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T): string\n"
        "    return LUtil.Show<T>(77);\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(Util.Go<A>(new A()));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.compiled);
    BEZEL_EXPECT_TRUE(
        r.err.find("does not implicitly convert to the type parameter `T`") !=
        std::string::npos);
}

// Explicit concrete argument: `Show<string>(77)` checks `77` against `string`.
BEZEL_TEST(xmod_explicit_typearg_arg_check_4915,
           imported_explicit_string_type_arg_int_rejected_spec_14_5_5_1) {
    auto r = build4915x(
        "module App4915x;\n"
        "import Lib4915x;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    Console.WriteLine(LUtil.Show<string>(77));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_FALSE(r.compiled);
    BEZEL_EXPECT_TRUE(
        r.err.find("the call to `Show`: argument 1 has a type that is not "
                   "implicitly convertible to the target type (§5.1)") !=
        std::string::npos);
}

// ═════════ Accept rows (RUN, exact exit code) ══════════════════════════════

// Inferred same-name call: the callee's `T` is still an inference slot
// (§14.5 infers `int`), so the #4915 exemption applies across the boundary.
BEZEL_TEST(xmod_explicit_typearg_arg_check_4915,
           imported_inferred_same_name_call_in_generic_caller_runs_spec_14_5) {
    auto r = build4915x(
        "module App4915x;\n"
        "import Lib4915x;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T): int\n"
        "    return LUtil.Echo(40) + 2;\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    return Util.Go<int>(1);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Explicit `<int>` inside a generic caller: the substituted formal is `int`,
// identity (§5.1) — the substitution must not over-reject.
BEZEL_TEST(xmod_explicit_typearg_arg_check_4915,
           imported_explicit_int_type_arg_in_generic_caller_runs_spec_14_5) {
    auto r = build4915x(
        "module App4915x;\n"
        "import Lib4915x;\n"
        "public class Util\n"
        "  public static method Go<T>(v: T): int\n"
        "    return LUtil.Echo<int>(41) + 1;\n"
        "  end Go;\n"
        "end Util;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    return Util.Go<int>(1);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.libOk);
    BEZEL_EXPECT_TRUE(r.compiled);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

}  // namespace bezel::module
