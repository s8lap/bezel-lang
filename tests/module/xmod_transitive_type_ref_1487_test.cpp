// fix #1487 / §2.5 / §2.8.3 (v0.196): a method call on a TRANSITIVELY-imported
// class must resolve regardless of the order dependencies are presented on the
// command line.
//
// Repro (three modules):
//   LibB  declares `Payload` with `Describe(): string` -> "payload".
//   LibM  imports LibB and exports `Maker.Make(): Payload` (`return new Payload()`).
//   App   imports LibM only and does `Maker.Make().Describe()` — §2.8.3 rule 2
//         (use of a transitively-reached public type without importing its
//         declaring module).
//
// Bug: the driver loaded each `--import` artifact in full (shells + members) in
// sequence. When LibM (which references `Payload` in Make's return type) was
// listed BEFORE LibB (which declares `Payload`), `Payload` was not yet
// registered, so the reconstructed return TypeRef fell to the
// declaration==nullptr / importedName-only path meant for genuinely-external
// builtins (§29.3 / #495). Codegen then lowered that call to a garbage value:
// `Describe()` returned an empty/garbage string (`.Length` == 0, prints "0")
// with NO diagnostic — a silent wrong answer, violating §2.8.3 rule 2/rule 3
// (all descriptions of one type denote the same type and must agree). Listing
// LibB first happened to work, so the defect was import-order-dependent.
//
// Fix: the driver now loads the whole `--import` set in two GLOBAL passes (all
// exported-type shells registered, then all members filled — module::
// loadDependencies), so a Named reference binds by name whatever the load order.
//
// These need the FULL compile + link + run pipeline (the bug is in cross-module
// metadata reconstruction + codegen), so they use compileObject + compileFile-
// with-deps and assert the run result. `Describe()` returns the string
// "payload"; Main returns its `.Length` (7) so the observable rides the exit
// code — before the fix (buggy order) this was 0.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

struct TempFile1487 {
    std::string path;
    explicit TempFile1487(std::string p) : path(std::move(p)) {}
    ~TempFile1487() { std::remove(path.c_str()); }
};

void write1487(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj1487(const std::string& src, const std::string& obj,
                    const std::vector<bezel::Dependency>& deps = {}) {
    bezel::Driver driver;
    return driver.compileObject(src, obj, deps).success;
}

// Compile the App importing LibM+LibB (objects listed in `depOrder`), run it,
// and return the exit code (-1 on compile/link/run failure, -2 to signal the
// dependency toolchain was unavailable so the caller can skip).
int buildAndRun1487(const std::string& stem, bool libMFirst) {
    auto dir = fs::temp_directory_path();

    TempFile1487 bSrc((dir / (stem + "_libb.bzl")).string());
    TempFile1487 bObj((dir / (stem + "_libb.o")).string());
    TempFile1487 mSrc((dir / (stem + "_libm.bzl")).string());
    TempFile1487 mObj((dir / (stem + "_libm.o")).string());
    TempFile1487 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile1487 aExe((dir / (stem + "_app")).string());

    // LibB: declares the transitively-reached type.
    write1487(bSrc.path,
        "module LibB; "
        "public class Payload "
        "  public method Describe(): string => \"payload\"; "
        "end Payload;");
    // LibM: imports LibB and returns a Payload from a factory.
    write1487(mSrc.path,
        "module LibM; "
        "import LibB; "
        "public class Maker "
        "  public static method Make(): Payload => new Payload(); "
        "end Maker;");
    // App: imports LibM only; USES Payload via Make() without naming it
    // (§2.8.3 rule 2). Returns the length of Describe()'s result (7 = "payload").
    write1487(aSrc.path,
        "module App; "
        "import LibM; "
        "class Program "
        "  public static method Main(): int "
        "    using var p := Maker.Make(); "
        "    var s := p.Describe(); "
        "    return s.Length; "
        "  end Main; "
        "end Program;");

    // libb.o (no deps).
    if (!compileObj1487(bSrc.path, bObj.path)) return -2;
    if (!std::ifstream(bObj.path).good()) return -2;
    // libm.o imports libb.o.
    std::vector<bezel::Dependency> mDeps;
    {
        bezel::Dependency d;
        d.metadataPath = bObj.path;
        d.objectPath = bObj.path;
        mDeps.push_back(d);
    }
    if (!compileObj1487(mSrc.path, mObj.path, mDeps)) return -2;
    if (!std::ifstream(mObj.path).good()) return -2;

    // App imports LibM+LibB, presenting the objects in the requested order. The
    // buggy order is libMFirst=true (LibM's Payload reference precedes LibB's
    // declaration in the load sequence).
    std::vector<bezel::Dependency> aDeps;
    auto pushDep = [&](const std::string& obj) {
        bezel::Dependency d;
        d.metadataPath = obj;
        d.objectPath = obj;
        aDeps.push_back(d);
    };
    if (libMFirst) { pushDep(mObj.path); pushDep(bObj.path); }
    else           { pushDep(bObj.path); pushDep(mObj.path); }

    bezel::Driver driver;
    if (!driver.compileFile(aSrc.path, aExe.path, aDeps).success) return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// The canonical repro: dependencies presented in the ORDER THAT BROKE (LibM,
// which references Payload, before LibB, which declares it). Before the fix this
// returned 0 (Describe() -> garbage/empty string); after, it returns 7 ("payload").
// §2.5 / §2.8.3 rule 2 (use of a transitively-reached type) + rule 3 (one type
// identity across artifacts).
BEZEL_TEST(xmod_transitive_type_ref_1487, libm_before_libb_resolves_2_8_3) {
    int r = buildAndRun1487("bz1487_mfirst_" + std::to_string(std::rand()),
                            /*libMFirst=*/true);
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 7);
}

// Order-independence canary: the reverse order (LibB before LibM) already worked
// before the fix; it must keep working after. Same value, 7.
BEZEL_TEST(xmod_transitive_type_ref_1487, libb_before_libm_still_resolves_2_8_3) {
    int r = buildAndRun1487("bz1487_bfirst_" + std::to_string(std::rand()),
                            /*libMFirst=*/false);
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 7);
}
