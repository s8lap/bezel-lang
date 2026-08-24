// fix #3238 / §12.5 / §12.3 / §23: `base.P` and `base[i]` on an IMPORTED
// abstract base property/indexer must carry the same `abstract base
// property …` / `abstract base indexer …` diagnostic as the local twin
// (#2838 / #2832).
//
// Pre-#3238, the two sema gates
//   - ExprTypecheck_Calls.cpp:18110 named-property form
//   - ExprTypecheck_Exprs.cpp:2983 indexer form
// read ONLY the ast::PropertyDecl's modifiers (the decl-null escape hatch
// every AST-reading named-property check in this file carries — see the
// `!pd`/scope comments around L18106 and L2979). An imported
// PropertySymbol has no `decl`, so the import-time `ps->modifiers` (set by
// ModuleLoader::convertMember at ModuleLoader.cpp:851 from the metadata)
// was never consulted, and the gate was a no-op on the cross-module
// shape. The compiled program then reached codegen's #3199 / #2837
// cross-module property/indexer ABI emits:
//
//   cannot lower `base.P` against the imported virtual property on
//   `Base` — §12.3 requires the base class's own implementation, but
//   the cross-module property ABI exports a direct entry point only
//   for a NON-virtual property … (#3199)
//
//   cannot lower `base[...]` against the imported virtual indexer on
//   `Base` — §12.3 requires the base class's own implementation, but
//   the cross-module indexer ABI exports a direct entry point only
//   for a NON-virtual indexer … (#2837)
//
// — citing §12.3 and the ABI stub rather than §12.5 and the abstract-base
// contract. The §12.5 reject is the more informative message (it names the
// property/indexer and the "it has no implementation" rationale), and
// it's the message the local twin (#2838 / #2832 tests in
// base_abstract_property_2838_test.cpp and base_indexer_nonvirtual_2832_test.cpp)
// already emits. The fix mirrors the named-method form's symbol-first
// modifier scan (ExprTypecheck_Calls.cpp:2373-2379, #874) into the two
// property/indexer sites.
//
// THREE TWO-MODULE AOT CASES, each a real `bezelc -c lib.bzl -o lib.o;
// bezelc --import lib.o app.bzl` cycle, asserting the §12.5 message text
// in the captured stderr:
//
//   T1. base.P        on imported abstract  property → "abstract base property"
//   T2. base[i]       on imported abstract  indexer  → "abstract base indexer"
//   T3. base.P        on imported CONCRETE virtual property ACCEPTED (control —
//                     the §12.3 bypass is the valid form for a non-abstract base)
//
// And one SINGLE-MODULE control that pins the local twin still rejects
// with the same message post-#3238 (so the fix didn't flip the local
// path):
//
//   T4. base.P on LOCAL abstract property → "abstract base property"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"

namespace bezel::sema {
namespace {

namespace fs = std::filesystem;

const fs::path& wtDir3238() {
    return wtScratchDir("wt-3238");
}

struct TempFile3238 {
    std::string path;
    explicit TempFile3238(std::string p) : path(std::move(p)) {}
    ~TempFile3238() { std::remove(path.c_str()); }
};

void writeSource3238(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Same exit-code contract as the xmod #3161 helper. The regression
// assertions read the §12.5 message TEXT out of `*errOut` (not just
// `code == -1`) so an unrelated failure mode (e.g. the import path
// silently regressing to a "cross-module ABI" reject) can never look
// like a passing test.
std::pair<int, std::string> compileImportRun3238(const std::string& libSrc,
                                                 const std::string& appSrc,
                                                 const std::string& tag,
                                                 std::string* errOut = nullptr) {
    auto dir = wtDir3238();
    std::string stem = "bz3238_" + tag + "_" + unique_stem("");
    TempFile3238 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3238 aObj((dir / (stem + "_a.o")).string());
    writeSource3238(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return {-2, ""};
    }
    if (!std::ifstream(aObj.path).good()) return {-2, ""};

    TempFile3238 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3238 bExe((dir / (stem + "_b")).string());
    writeSource3238(bSrc.path, appSrc);
    TempFile3238 bErr((dir / (stem + "_b.err")).string());
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;
        bool ok = false;
        if (errOut) {
            std::fflush(stderr);
            int saved = ::dup(STDERR_FILENO);
            int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                              0644);
            if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
            std::fflush(stderr);
            if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
            std::ifstream in(bErr.path);
            std::ostringstream ss;
            ss << in.rdbuf();
            *errOut = ss.str();
        } else {
            ok = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                 deps, opts).success;
        }
        if (!ok) return {-1, ""};
    }
    std::string cmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

std::pair<int, std::string> compileRunLocal3238(const std::string& src,
                                                const std::string& tag,
                                                std::string* errOut = nullptr) {
    auto dir = wtDir3238();
    std::string stem = "bz3238l_" + tag + "_" + unique_stem("");
    TempFile3238 aSrc((dir / (stem + ".bzl")).string());
    TempFile3238 aExe((dir / stem).string());
    TempFile3238 aErr((dir / (stem + ".err")).string());
    writeSource3238(aSrc.path, src);
    bezel::Driver driver;
    bool okLocal = false;
    if (errOut) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(aErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                          0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(aErr.path);
        std::ostringstream ss;
        ss << in.rdbuf();
        *errOut = ss.str();
    } else {
        okLocal = bezel::test::compileFileForTest(
                      driver, aSrc.path, aExe.path, {},
                      codegen::CodegenOptions{}).success;
    }
    if (!okLocal) return {-1, ""};
    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {-1, ""};
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

bool has3238(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The cross-module library — an abstract base with both an abstract
// property and an abstract indexer. Mirrors the issue's own repro.
const char* kLib3238 =
    "module M3238; "
    "public abstract class Base3238 "
    "  public abstract property P3238: int; "
    "  public abstract property [i: int]: int; "
    "end Base3238; ";

// T1 — IMPORTED `base.P` on imported abstract property must emit the
//      §12.5 "abstract base property" message, NOT the codegen-time #3199
//      "imported virtual property" cross-module ABI message.
const char* kT1App3238 =
    "module A3238t1; import M3238; "
    "class Local3238: M3238.Base3238 "
    "  public override property P3238: int => 42; "
    "  public override property [i: int]: int => i; "
    "  public method ViaBaseProp(): int => base.P3238; "
    "end Local3238; "
    "public class Driver3238 "
    "  public static method Main(): int "
    "    var l := new Local3238(); return l.ViaBaseProp(); "
    "  end Main; "
    "end Driver3238; ";

// T2 — IMPORTED `base[i]` on imported abstract indexer must emit the
//      §12.5 "abstract base indexer" message, NOT the codegen-time #2837
//      "imported virtual indexer" cross-module ABI message.
const char* kT2App3238 =
    "module A3238t2; import M3238; "
    "class Local3238: M3238.Base3238 "
    "  public override property P3238: int => 42; "
    "  public override property [i: int]: int => i; "
    "  public method ViaBaseIndex(i: int): int => base[i]; "
    "end Local3238; "
    "public class Driver3238 "
    "  public static method Main(): int "
    "    var l := new Local3238(); return l.ViaBaseIndex(1); "
    "  end Main; "
    "end Driver3238; ";

// T3 — IMPORTED non-virtual concrete property: `base.P` is the §12.3 valid
//      bypass (the imported ABI for a NON-virtual property exports a direct
//      entry point, so `base.P` reaches the base class's own value). MUST
//      COMPILE AND RUN. Pins that the fix didn't widen the abstract scan
//      into "every base access in this position is rejected".
//      (The IMPORTED virtual case is the codegen-time #3199 reject — a
//      different concern not covered here; we deliberately use a
//      non-virtual base with NO override to keep T3 a clean ACCEPT control.)
const char* kLib3238Concrete =
    "module M3238c; "
    "public class Base3238c "
    "  public property P3238: int => 100; "
    "end Base3238c; ";

const char* kT3App3238 =
    "module A3238t3; import M3238c; "
    "class Local3238c: M3238c.Base3238c "
    "  public method ViaBaseProp(): int => base.P3238; "
    "end Local3238c; "
    "public class Driver3238c "
    "  public static method Main(): int "
    "    var l := new Local3238c(); return l.ViaBaseProp(); "
    "  end Main; "
    "end Driver3238c; ";

// T4 — LOCAL (single-module) `base.P` on an abstract property still
//      rejects with the §12.5 message. The T1/T2 fix is structural —
//      read the symbol modifiers — so the local path must keep
//      rejecting with byte-identical text. (Mirrors the T2 control in
//      the xmod #3161 test.)
const char* kT4Local3238 =
    "module M3238t4; "
    "abstract class B "
    "  public abstract property P: int; "
    "end B; "
    "class D: B "
    "  public override property P: int => 7; "
    "  public method ViaBaseProp(): int => base.P; "
    "end D; "
    "public class Program "
    "  public static method Main(): int "
    "    var d := new D(); return d.ViaBaseProp(); "
    "  end Main; "
    "end Program; ";

}  // namespace

// T1 — IMPORTED abstract property → §12.5 "abstract base property"
BEZEL_TEST(sema_base_abstract_xmod_3238,
           imported_abstract_base_property_via_base_emits_abstract_base_message) {
    std::string err;
    auto r = compileImportRun3238(kLib3238, kT1App3238, "t1", &err);
    if (r.first == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r.first, -1);
    BEZEL_EXPECT_TRUE(has3238(err, "abstract base property"));
    BEZEL_EXPECT_TRUE(has3238(err, "no implementation"));
    BEZEL_EXPECT_TRUE(has3238(err, "(\xC2\xA7""12.3/\xC2\xA7""12.5)"));
    // Negative regression guard: must NOT carry the codegen-time #3199
    // "imported virtual property" ABI message (the fall-through that
    // pre-#3238 emitted).
    BEZEL_EXPECT_FALSE(has3238(err, "imported virtual property"));
}

// T2 — IMPORTED abstract indexer → §12.5 "abstract base indexer"
BEZEL_TEST(sema_base_abstract_xmod_3238,
           imported_abstract_base_indexer_via_base_emits_abstract_base_message) {
    std::string err;
    auto r = compileImportRun3238(kLib3238, kT2App3238, "t2", &err);
    if (r.first == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r.first, -1);
    BEZEL_EXPECT_TRUE(has3238(err, "abstract base indexer"));
    BEZEL_EXPECT_TRUE(has3238(err, "no implementation"));
    BEZEL_EXPECT_TRUE(has3238(err, "(\xC2\xA7""12.3/\xC2\xA7""12.5)"));
    // Negative regression guard: must NOT carry the codegen-time #2837
    // "imported virtual indexer" ABI message.
    BEZEL_EXPECT_FALSE(has3238(err, "imported virtual indexer"));
}

// T3 — IMPORTED NON-virtual concrete property: `base.P` is the §12.3 valid
//      bypass (the imported ABI for a non-virtual property exports a direct
//      entry point, so `base.P` reaches the base class's own value).
//      Must compile and run; exit status = the Main's return value, here
//      100 — bypass reaches the base class's own `100`, NOT anything the
//      override would have computed. (The IMPORTED virtual case is the
//      #3199 reject — different concern, not this fix's scope.)
BEZEL_TEST(sema_base_abstract_xmod_3238,
           imported_concrete_base_property_via_base_still_accepted_12_3) {
    std::string err;
    auto r = compileImportRun3238(kLib3238Concrete, kT3App3238, "t3", &err);
    if (r.first == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_TRUE(r.first != -1);
    BEZEL_EXPECT_EQ(r.first, 100);
}

// T4 — LOCAL abstract property twin pins the local-side path still
//      rejects post-#3238 with the same §12.5 text. Mirrors the
//      base_abstract_property_2838_test.cpp
//      `abstract_base_property_via_base_is_rejected` gate but is exercised
//      here to catch any silent regression on the local arm of the fix.
BEZEL_TEST(sema_base_abstract_xmod_3238,
           local_abstract_base_property_via_base_still_rejects_12_5) {
    std::string err;
    auto r = compileRunLocal3238(kT4Local3238, "t4", &err);
    if (r.first == -2) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r.first, -1);
    BEZEL_EXPECT_TRUE(has3238(err, "abstract base property"));
    BEZEL_EXPECT_TRUE(has3238(err, "(\xC2\xA7""12.3/\xC2\xA7""12.5)"));
    BEZEL_EXPECT_FALSE(has3238(err, "imported virtual property"));
}

}  // namespace bezel::sema
