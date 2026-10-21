// #6654: the driver's self-contained link mode — the logic that needs no real
// toolchain bundle.
//
// A release tarball ships lib/bezel/x86_64-linux-musl/ (a static ld.lld, musl,
// libc++/libc++abi/libunwind, compiler-rt builtins and a musl-built runtime)
// next to bin/bezelc, and bezelc then links a fully static executable with no
// clang. The real bundle exists only in a release build, so its end-to-end
// check (hello, a deep Domain.Spawn recursion, exceptions, Task.Delay,
// formatting, in a container with no compiler) is release_static.sh's smoke
// test. These rows pin what can be checked here:
//   * the recipe itself (Driver::selfContainedLinkArgs, a pure function),
//     against a fake bundle path;
//   * that LinkMode::SelfContained really RUNS that recipe — the bundle's
//     bin/ld.lld, with exactly those arguments, and nothing from PATH —
//     using a fake bundle whose ld.lld is a recording shell script;
//   * that a failing bundle linker fails the build;
//   * that the dev build has no bundle next to its binaries, so the default
//     stays the hosted recipe (LinkMode::System) for the suite.
// The CLI surface (--link=..., the --sanitize=address error, --help) is on
// the lit track: tests/lit/driver_link_mode_6654_*.bzl.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "driver/Driver.hpp"
#include "runtime/bezel_thread_stack.h"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace fs = std::filesystem;
using bezel::Driver;
using bezel::LinkMode;

namespace {

const char* kHello =
    "module SelfContained;\n"
    "\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    Console.WriteLine(\"hi\");\n"
    "  end Main;\n"
    "end Program;\n";

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A fake bundle under `root` whose bin/ld.lld records its arguments (one per
// line) into `root`/argv.txt, then exits with `rc` — creating the -o output
// first when rc is 0, as a real linker would.
fs::path makeFakeBundle(const fs::path& root, int rc) {
    const fs::path bundle = root / "lib" / "bezel" / "x86_64-linux-musl";
    fs::create_directories(bundle / "bin");
    const fs::path ld = bundle / "bin" / "ld.lld";
    std::ofstream(ld) << "#!/bin/sh\n"
                         ": > '" << (root / "argv.txt").string() << "'\n"
                         "out=\n"
                         "prev=\n"
                         "for a in \"$0\" \"$@\"; do\n"
                         "  printf '%s\\n' \"$a\" >> '" << (root / "argv.txt").string() << "'\n"
                         "  [ \"$prev\" = -o ] && out=\"$a\"\n"
                         "  prev=\"$a\"\n"
                         "done\n"
                      << (rc == 0 ? "[ -n \"$out\" ] && : > \"$out\"\n" : "")
                      << "exit " << rc << "\n";
    ::chmod(ld.c_str(), 0755);
    return bundle;
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string l; std::getline(ss, l);) out.push_back(l);
    return out;
}

}  // namespace

// The recipe the #6654 spike measured, in order: the static ld.lld, -static,
// the bundle as the -L search dir (for the archives' dependent-library hints),
// the 8 MiB default thread stack (BZ_THREAD_STACK_SIZE, #6695), crt1/crti,
// the program's objects, then runtime, C++ runtime, libc, builtins, crtn.
BEZEL_TEST(self_contained_link_6654, link_args_are_the_spike_recipe_against_the_bundle) {
    const std::vector<std::string> got = Driver::selfContainedLinkArgs(
        "/w/prog.o", "/w/prog", {"/w/dep1.o", "/w/dep2.o"}, "/opt/bz/lib/bezel/x86_64-linux-musl");
    const std::string B = "/opt/bz/lib/bezel/x86_64-linux-musl/";
    const std::vector<std::string> want = {
        B + "bin/ld.lld",
        "-static",
        "-L/opt/bz/lib/bezel/x86_64-linux-musl",
        "-z", "stack-size=8388608",
        "--eh-frame-hdr",
        "-z", "noexecstack",
        "-o", "/w/prog",
        B + "crt1.o",
        B + "crti.o",
        "/w/prog.o",
        "/w/dep1.o",
        "/w/dep2.o",
        B + "libbezel_runtime.a",
        B + "libc++.a",
        B + "libc++abi.a",
        B + "libunwind.a",
        B + "libc.a",
        B + "libclang_rt.builtins-x86_64.a",
        B + "crtn.o",
    };
    BEZEL_EXPECT_TRUE(got == want);
    if (got != want) {
        for (const auto& a : got) std::fprintf(stderr, "  got: %s\n", a.c_str());
    }
    // The stack size is the runtime's constant, not a second copy of it.
    BEZEL_EXPECT_EQ(std::string("stack-size=") + std::to_string(BZ_THREAD_STACK_SIZE),
                    std::string("stack-size=8388608"));
}

// LinkMode::SelfContained runs the bundle's own bin/ld.lld with exactly the
// recipe above, and the build succeeds when it does.
BEZEL_TEST(self_contained_link_6654, self_contained_mode_runs_the_bundle_linker) {
    const fs::path root = bezel_test::wtScratchDir("self_contained_link_6654_ok");
    const fs::path bundle = makeFakeBundle(root, 0);
    const fs::path src = root / "prog.bzl";
    std::ofstream(src) << kHello;
    const fs::path exe = root / "prog";

    Driver d;
    d.setLinkMode(LinkMode::SelfContained, bundle.string());
    const bezel::CompileResult r =
        bezel::test::compileFileForTest(d, src.string(), exe.string());
    BEZEL_EXPECT_TRUE(r.success);
    BEZEL_EXPECT_TRUE(fs::exists(exe));

    const std::vector<std::string> argv = lines(slurp(root / "argv.txt"));
    const std::vector<std::string> want = Driver::selfContainedLinkArgs(
        exe.string() + ".o", exe.string(), {}, bundle.string());
    BEZEL_EXPECT_TRUE(argv == want);
    if (argv != want) {
        for (const auto& a : argv) std::fprintf(stderr, "  ran: %s\n", a.c_str());
    }
    // The temporary object is removed after the link, as on the hosted path.
    BEZEL_EXPECT_TRUE(!fs::exists(exe.string() + ".o"));
}

// A bundle linker that fails fails the build (no silent fallback to clang).
BEZEL_TEST(self_contained_link_6654, failing_bundle_linker_fails_the_build) {
    const fs::path root = bezel_test::wtScratchDir("self_contained_link_6654_fail");
    const fs::path bundle = makeFakeBundle(root, 1);
    const fs::path src = root / "prog.bzl";
    std::ofstream(src) << kHello;
    const fs::path exe = root / "prog";

    Driver d;
    d.setLinkMode(LinkMode::SelfContained, bundle.string());
    const bezel::CompileResult r =
        bezel::test::compileFileForTest(d, src.string(), exe.string());
    BEZEL_EXPECT_TRUE(!r.success);
    BEZEL_EXPECT_TRUE(!fs::exists(exe));
    // It did run the bundle's linker (the failure is that linker's, not a
    // missing-file error from somewhere else).
    const std::vector<std::string> argv = lines(slurp(root / "argv.txt"));
    BEZEL_EXPECT_TRUE(!argv.empty());
    if (!argv.empty())
        BEZEL_EXPECT_EQ(argv.front(), (bundle / "bin" / "ld.lld").string());
}

// The bundle is looked up next to the real executable; the dev build has none
// there, so bezelc's default stays LinkMode::System and the suite's hosted
// link is unchanged.
BEZEL_TEST(self_contained_link_6654, dev_build_has_no_bundle_so_the_default_stays_system) {
    const std::string dir = Driver::selfContainedBundleDir();
    BEZEL_EXPECT_TRUE(!dir.empty());
    BEZEL_EXPECT_TRUE(fs::path(dir).is_absolute());
    const std::string suffix = "/lib/bezel/x86_64-linux-musl";
    BEZEL_EXPECT_TRUE(dir.size() > suffix.size() &&
                      dir.compare(dir.size() - suffix.size(), suffix.size(), suffix) == 0);
    BEZEL_EXPECT_TRUE(!Driver::selfContainedBundlePresent());
}
