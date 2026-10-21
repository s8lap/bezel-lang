// aot_link_mode_6706_test.cpp — the self-contained (musl) link knob of the AOT
// harness (#6706): BEZEL_AOT_LINK=self-contained + BEZEL_AOT_BUNDLE_DIR.
//
// Pinned here:
//   * the default path is untouched: withLinkMode is the identity on an empty
//     component, and a system-link build contributes an empty component, so no
//     KE / K2 key moves with the knob unset;
//   * the key components themselves: the bundle identity is content-addressed
//     (an identical copy elsewhere keys the same, a changed byte does not) and
//     the self-contained recipe is not the system recipe;
//   * end to end, in a re-executed child against a FAKE bundle whose bin/ld.lld
//     records its argv: under the knob runAot links through the bundle's
//     ld.lld with Driver::selfContainedLinkArgs's recipe, runAotChecked stays on
//     the system link, the SELF-CONTAINED LINK line counts both, and a product
//     the system link stored in the SAME cache is not served to the knob run
//     (the musl/glibc alias the key component exists to prevent);
//   * a misspelt BEZEL_AOT_LINK and a missing bundle abort the child loudly.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "support/aot_link_mode.hpp"
#include "support/aot_support.hpp"
#include "support/scratch_dir.hpp"
#include "test_harness.hpp"

namespace {

namespace aot = bezel::test::aot_detail;
namespace fs = std::filesystem;
using bezel::test::currentTestFailed;

constexpr const char* kProgram =
    "module M; class C public static method Main(): int "
    "  return 0; "
    "end Main; end C;";

constexpr const char* kChildGuard = "BEZEL_AOT_LINK_MODE_6706_CHILD";

std::string selfExe() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

std::string readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

// A fake bundle: every file selfContainedLinkArgs names exists (as a dummy),
// and bin/ld.lld is a script that appends its argv to `record` and fails, so a
// link through it is observable and never produces an executable.
fs::path makeFakeBundle(const fs::path& root, const fs::path& record) {
    const fs::path b = root / "x86_64-linux-musl";
    for (const char* f : {"crt1.o", "crti.o", "crtn.o", "libc.a", "libc++.a",
                          "libc++abi.a", "libunwind.a",
                          "libclang_rt.builtins-x86_64.a",
                          "libbezel_runtime.a"})
        writeFile(b / f, std::string("dummy ") + f + "\n");
    const fs::path ld = b / "bin" / "ld.lld";
    writeFile(ld, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + record.string() +
                      "'\nexit 1\n");
    ::chmod(ld.c_str(), 0755);
    return b;
}

struct ChildRun {
    int status = -1;
    bool timedOut = false;
    std::string out;
};

ChildRun runChild(const fs::path& dir, const std::string& tag,
                  std::vector<std::pair<std::string, std::string>> env) {
    ChildRun c;
    const std::string outPath = (dir / (tag + ".out")).string();
    env.insert(env.end(), {{"BEZEL_TEST_FILTER", "link_mode_6706_child_probe"},
                           {kChildGuard, "1"},
                           {"BEZEL_TEST_LIST", ""},
                           {"BEZEL_TEST_ENUMERATE", ""},
                           {"BEZEL_SHARD_COUNT", ""},
                           {"BEZEL_SHARD_INDEX", ""},
                           {"BEZEL_SHARD_SUMMARY", ""},
                           {"BEZEL_AOT_FORCE_SANITIZE", ""}});
    int execErrno = 0;
    c.status = aot::spawnCaptured({selfExe()}, outPath, outPath,
                                  /*timeoutSec=*/600, c.timedOut, env,
                                  &execErrno);
    c.out = readAll(outPath);
    return c;
}

std::size_t countLines(const std::string& s) {
    std::size_t n = 0;
    for (char ch : s) n += (ch == '\n');
    return n;
}

}  // namespace

// ---- pure seams -------------------------------------------------------------

BEZEL_TEST(aot_link_mode_6706, with_link_mode_is_identity_on_empty_component) {
    const std::string mode = "k2;sa=0;cd=1;cc=1;mm=1;sab=0;";
    BEZEL_EXPECT_EQ(aot::withLinkMode(mode, ""), mode);
    BEZEL_EXPECT_EQ(aot::withLinkMode(mode, "sc=abc"), mode + "lk:sc=abc;");
}

BEZEL_TEST(aot_link_mode_6706, system_link_builds_contribute_no_key_component) {
    bezel::codegen::CodegenOptions plain;
    bezel::codegen::CodegenOptions asan;
    asan.sanitizeAddress = true;
    // A sanitized build is on the system link in every mode.
    BEZEL_EXPECT_EQ(aot::linkModeKeyText(asan), std::string());
    BEZEL_EXPECT_TRUE(aot::modeString(asan).find("lk:") == std::string::npos);
    if (!bezel::test::aotLinkSelfContained()) {
        // The default: nothing moves.
        BEZEL_EXPECT_EQ(aot::linkModeKeyText(plain), std::string());
        BEZEL_EXPECT_TRUE(aot::modeString(plain).find("lk:") ==
                          std::string::npos);
        BEZEL_EXPECT_FALSE(aot::useSelfContainedLink(plain));
    } else {
        BEZEL_EXPECT_TRUE(aot::modeString(plain).find("lk:sc=") !=
                          std::string::npos);
        // A marker puts the build back on the system link, and its key with it.
        BEZEL_SELF_CONTAINED_DIVERGES(6706, "self-test of the marker");
        BEZEL_EXPECT_FALSE(aot::useSelfContainedLink(plain));
        BEZEL_EXPECT_EQ(aot::linkModeKeyText(plain), std::string());
    }
}

BEZEL_TEST(aot_link_mode_6706, bundle_identity_is_content_addressed) {
    const fs::path dir = wtScratchDir("link-mode-6706-ident");
    const fs::path a = makeFakeBundle(dir / "a", dir / "unused.rec");
    const fs::path b = makeFakeBundle(dir / "b", dir / "unused.rec");
    const std::string ia = aot::bundleIdentityFrom(a.string());
    BEZEL_EXPECT_EQ(ia.size(), std::size_t{64});
    // Same bytes at another path: same identity (the path is not keyed).
    BEZEL_EXPECT_EQ(aot::bundleIdentityFrom(b.string()), ia);
    // One changed byte in libc.a: a different identity.
    writeFile(b / "libc.a", "dummy libc.a!\n");
    BEZEL_EXPECT_TRUE(aot::bundleIdentityFrom(b.string()) != ia);
}

BEZEL_TEST(aot_link_mode_6706, self_contained_recipe_differs_from_system_recipe) {
    const std::string r1 = aot::selfContainedRecipeSignatureFor("id1");
    BEZEL_EXPECT_TRUE(r1 != aot::selfContainedRecipeSignatureFor("id2"));
    BEZEL_EXPECT_TRUE(r1 != bezel::Driver::linkRecipeSignature(false));
    BEZEL_EXPECT_TRUE(r1.find("<BUNDLE>/bin/ld.lld") != std::string::npos);
    BEZEL_EXPECT_TRUE(r1.find("-static") != std::string::npos);
}

// ---- end to end, in a child -------------------------------------------------

// The probe the parents re-execute. Outside a parent it is an ordinary runAot
// row on whatever link the process was given.
BEZEL_TEST(aot_link_mode_6706, link_mode_6706_child_probe) {
    auto r = bezel::test::runAot(kProgram);
    std::cout << "PROBE runAot ran=" << (r.ran ? 1 : 0)
              << " exit=" << r.exitCode << "\n";
    auto c = bezel::test::runAotChecked(kProgram);
    std::cout << "PROBE runAotChecked ran=" << (c.ran ? 1 : 0)
              << " exit=" << c.exitCode << "\n";
    // The checked leg is on the system link in every mode, so it must run.
    BEZEL_EXPECT_TRUE(c.ran);
    if (!std::getenv(kChildGuard)) BEZEL_EXPECT_TRUE(r.ran);
}

BEZEL_TEST(aot_link_mode_6706, knob_links_through_the_bundle_and_never_aliases) {
    if (const char* g = std::getenv(kChildGuard); g && *g) return;
    BEZEL_EXPECT_FALSE(selfExe().empty());

    const fs::path dir = wtScratchDir("link-mode-6706-e2e");
    const fs::path record = dir / "ld.record";
    const fs::path bundle = makeFakeBundle(dir / "bundle", record);
    const std::string cache = (dir / "cache").string();

    // 1. The system link fills a private cache with this program's products.
    ChildRun sys = runChild(dir, "system",
                            {{"BEZEL_AOT_LINK", ""},
                             {"BEZEL_AOT_BUNDLE_DIR", ""},
                             {"BEZEL_AOT_CACHE", "1"},
                             {"BEZEL_AOT_CACHE_DIR", cache}});
    BEZEL_EXPECT_FALSE(sys.timedOut);
    BEZEL_EXPECT_TRUE(sys.out.find("PROBE runAot ran=1 exit=0") !=
                      std::string::npos);
    // Default: no line, nothing recorded.
    BEZEL_EXPECT_TRUE(sys.out.find("SELF-CONTAINED LINK") == std::string::npos);
    BEZEL_EXPECT_FALSE(fs::exists(record));

    // 2. The knob, SAME cache: the glibc product must not be served; the link
    //    must go through the bundle's ld.lld (which fails, by design).
    ChildRun sc = runChild(dir, "self-contained",
                           {{"BEZEL_AOT_LINK", "self-contained"},
                            {"BEZEL_AOT_BUNDLE_DIR", bundle.string()},
                            {"BEZEL_AOT_CACHE", "1"},
                            {"BEZEL_AOT_CACHE_DIR", cache}});
    BEZEL_EXPECT_FALSE(sc.timedOut);
    BEZEL_EXPECT_TRUE(sc.out.find("[ RUN  ] aot_link_mode_6706.link_mode_6706_child_probe") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(sc.out.find("PROBE runAot ran=0") != std::string::npos);
    BEZEL_EXPECT_TRUE(sc.out.find("PROBE runAotChecked ran=1 exit=0") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(
        sc.out.find("SELF-CONTAINED LINK: 1 run(s), 1 sanitized run(s) kept on "
                    "system link, 0 marked-divergent run(s)") !=
        std::string::npos);
    const std::string rec = readAll(record.string());
    // Exactly ONE link through the bundle (the checked leg did not use it), and
    // it is selfContainedLinkArgs's recipe against this bundle.
    BEZEL_EXPECT_EQ(countLines(rec), std::size_t{1});
    BEZEL_EXPECT_TRUE(rec.find("-static -L" + bundle.string() + " ") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(rec.find("-z stack-size=") != std::string::npos);
    BEZEL_EXPECT_TRUE(rec.find(bundle.string() + "/crt1.o") != std::string::npos);
    BEZEL_EXPECT_TRUE(rec.find(bundle.string() + "/libc.a") != std::string::npos);

    if (currentTestFailed())
        std::cout << "--- system child ---\n" << sys.out
                  << "\n--- self-contained child ---\n" << sc.out
                  << "\n--- ld record ---\n" << rec << "\n--- end ---\n";
}

BEZEL_TEST(aot_link_mode_6706, bad_knob_values_abort_loudly) {
    if (const char* g = std::getenv(kChildGuard); g && *g) return;
    const fs::path dir = wtScratchDir("link-mode-6706-bad");

    ChildRun typo = runChild(dir, "typo", {{"BEZEL_AOT_LINK", "musl"}});
    BEZEL_EXPECT_TRUE(typo.out.find("BEZEL_AOT_LINK='musl' is not a link mode") !=
                      std::string::npos);
    BEZEL_EXPECT_FALSE(WIFEXITED(typo.status) && WEXITSTATUS(typo.status) == 0);

    ChildRun nodir = runChild(dir, "nodir",
                              {{"BEZEL_AOT_LINK", "self-contained"},
                               {"BEZEL_AOT_BUNDLE_DIR", ""}});
    BEZEL_EXPECT_TRUE(nodir.out.find("needs BEZEL_AOT_BUNDLE_DIR") !=
                      std::string::npos);
    BEZEL_EXPECT_FALSE(WIFEXITED(nodir.status) && WEXITSTATUS(nodir.status) == 0);

    ChildRun nold = runChild(dir, "nold",
                             {{"BEZEL_AOT_LINK", "self-contained"},
                              {"BEZEL_AOT_BUNDLE_DIR", (dir / "empty").string()}});
    BEZEL_EXPECT_TRUE(nold.out.find("has no bin/ld.lld") != std::string::npos);
    BEZEL_EXPECT_FALSE(WIFEXITED(nold.status) && WEXITSTATUS(nold.status) == 0);

    if (currentTestFailed())
        std::cout << "--- typo ---\n" << typo.out << "\n--- nodir ---\n"
                  << nodir.out << "\n--- nold ---\n" << nold.out << "\n";
}
