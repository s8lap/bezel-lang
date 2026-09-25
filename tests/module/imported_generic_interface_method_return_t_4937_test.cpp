// fix #4937 (§14.1 / §2.5 / §2.8.2 / §14.2, v0.307): a method called
// through an IMPORTED GENERIC INTERFACE left the method's return type
// as the bare type parameter `T`. The byte-identical single-module
// program was correct (`dog dog dog end`); the cross-module form
// printed `dog end` only — the two interface-receiver calls printed
// nothing because their member-access chain on the bare-T return
// silently degraded (#2477 "cannot dispatch member on receiver of type
// T"; #3079 "argument failed to lower to a value, so the whole
// statement emits nothing").
//
// THE DEFECT. When `app.bzl` declares `var io: IO<Dog> := k; io.Peek()`,
// the call lookup walks the IMPORTED interface `IO<T>` and reads
// `Peek(): T`, but does NOT apply the import-side instantiation
// `{T := Dog}` to the return type. Codegen then types the call's
// return as the bare `T`. §14.1's substitution needed the interface's
// instantiation map carried across the artifact boundary into the
// method-return-type site the importer reads from.
//
// Note: the CLASS-receiver case `k.Peek()` (with `k: Kennel`, where
// `Kennel: IO<Dog>`) does apply the parent substitution — the gap was
// specifically the IMPORTED-method path on typeCall. The local-method
// path runs `substituteMemberType(rt, ma->target.get(), ms->parent)`
// for both class and interface receivers (`parent` is the interface
// `IO<T>` for `Peek`); the imported-method path sat below that
// substitution.
//
// THE FIX (this commit) applies the same gate to the imported-method
// arm: after the imported-method argument-driven substitution
// (#4611) and the explicit-list substitution (#4622) have run, apply
// `substituteMemberType(callTy, callee.target, ms->parent)` BEFORE
// `setExprType(&ce, callTy)`. Gated on `ms->parent` having generic
// params (the helper's own early-out yields the bare substitution
// otherwise, so non-generic receivers stay a no-op).
//
// Rows:
//
//   (1) ACCEPT — xmod: the issue's repro distilled to one WriteLine:
//         `var io: IO<Dog> := k; Console.WriteLine(io.Peek().Name());`
//       Pre-fix FAILS: prints blank on this line (the bare-T `Peek`
//       return has no `Name` to dispatch). Post-fix ACCEPTS and
//       prints `dog`.
//   (2) ACCEPT — xmod: `var ia: IO<Animal> := k; ia.Peek().Name()` —
//       covariant `T := Animal` binding via the `out T` modifier.
//       Pre-fix FAILS the same way. Post-fix ACCEPTS and prints `dog`
//       (since the contained Dog.Name() returns dog).
//   (3) ACCEPT — xmod CONTROL: the CLASS-receiver call
//       `k.Peek().Name()` — the issue itself notes already works.
//       Both pre- and post-fix ACCEPT and print `dog`. Pins that the
//       fix did not regress the class-receiver sibling.
//   (4) ACCEPT — single-module CONTROL: the BYTE-IDENTICAL program
//       but with lib and app concatenated into `module M`. Pre-fix
//       and post-fix both print `dog dog dog end`. Pins that
//       single-module path continues to work after the fix.
//
// Per [[feedback-test-oracles]]: rows 1-2 pin the SPECIFIC defective
// shape (cross-module interface-receiver method-return); row 3
// pins the class-receiver SIBLING; row 4 pins single-module
// (`@1b33779e` already fixed the local path). The fix's narrow
// blast-radius mean no other rows would be expected to flip; the
// §18 fallout sweep in STEP 6 verifies.
//
// Per [[feedback-reviewer-must-run-accept-rows]] ★★★ the ACCEPT
// rows use the compiled-binary run after a real Driver compileFile +
// link + native exec.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

struct TempFile4937 {
    std::string path;
    explicit TempFile4937(std::string p) : path(std::move(p)) {}
    ~TempFile4937() { std::remove(path.c_str()); }
};

void writeSource4937(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Subprocess-run-with-stdout-capture for the xmod path. Returns -2 if
// the dependency toolchain was unavailable (test skips), -1 if the
// program did not build or did not exit normally, otherwise the
// program's exit code with stdout in *outStdout.
int compileImportRunCapture4937(const std::string& libSrc,
                                const std::string& appSrc,
                                std::string* outStdout) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4937_");
    TempFile4937 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile4937 lObj((dir / (stem + "_lib.o")).string());
    writeSource4937(lSrc.path, libSrc);
    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -2;
    if (!std::ifstream(lObj.path).good()) return -2;

    TempFile4937 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile4937 aExe((dir / stem).string());
    writeSource4937(aSrc.path, appSrc);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath   = lObj.path;
    deps.push_back(d);
    if (!driver.compileFile(aSrc.path, aExe.path, deps).success) return -1;

    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return -1;
    std::array<char, 256> buf{};
    std::string captured;
    while (true) {
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        captured.append(buf.data(), n);
    }
    int rc = pclose(f);
    if (outStdout) *outStdout = captured;
    if (rc == -1 || !WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

// lib.bzl from the issue body.
const char* kLib4937 =
    "module Lib4937; "
    "public class An4937 "
    "  public constructor() end constructor; "
    "  public virtual method Name(): string => \"animal\"; "
    "end An4937; "
    "public class Dog4937: An4937 "
    "  public constructor() base(); end constructor; "
    "  public override method Name(): string => \"dog\"; "
    "end Dog4937; "
    "public interface IO4937<out T> "
    "  method Peek(): T; "
    "end IO4937; "
    "public class Ken4937: IO4937<Dog4937> "
    "  private owned _d: Dog4937; "
    "  public constructor() _d := new Dog4937(); end constructor; "
    "  public method Peek(): Dog4937 => _d; "
    "end Ken4937; ";

std::string kApp4937(const std::string& body) {
    return "module App4937; import Lib4937; "
           "public class P4937 "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end P4937; ";
}

// Single-module control (lib + app concatenated into `module M`).
const char* kSingleModule4937 =
    "module M4937; "
    "public class An4937s "
    "  public constructor() end constructor; "
    "  public virtual method Name(): string => \"animal\"; "
    "end An4937s; "
    "public class Dog4937s: An4937s "
    "  public constructor() base(); end constructor; "
    "  public override method Name(): string => \"dog\"; "
    "end Dog4937s; "
    "public interface IO4937s<out T> "
    "  method Peek(): T; "
    "end IO4937s; "
    "public class Ken4937s: IO4937s<Dog4937s> "
    "  private owned _d: Dog4937s; "
    "  public constructor() _d := new Dog4937s(); end constructor; "
    "  public method Peek(): Dog4937s => _d; "
    "end Ken4937s; "
    "public class P4937s "
    "  public static method Main(): int "
    "    using var k := new Ken4937s(); "
    "    Console.WriteLine(k.Peek().Name()); "
    "    var io: IO4937s<Dog4937s> := k; "
    "    Console.WriteLine(io.Peek().Name()); "
    "    var ia: IO4937s<An4937s> := k; "
    "    Console.WriteLine(ia.Peek().Name()); "
    "    Console.WriteLine(\"end\"); "
    "    return 0; "
    "  end Main; "
    "end P4937s; ";

}  // namespace

// (1) ACCEPT — xmod issue repro (interface receiver, IO<Dog>).
// Pre-fix: prints blank on this line (silent-wrong). Post-fix: prints
// `dog` then `end` (with `end` only as a separator — the issue's exact
// repro prints BOTH `dog` and `end` from a 2-line caller; we trip the
// fix on the explicit single-interface WriteLine to keep the row
// focused on the interface-receiver substitution path).
BEZEL_TEST(imported_generic_interface_method_return_t_4937,
           xmod_io_Dot_Peek_Dot_Name_prints_dog_then_end) {
    std::string stdoutText;
    int r = compileImportRunCapture4937(
        kLib4937,
        kApp4937(
            "    using var k := new Ken4937(); "
            "    var io: IO4937<Dog4937> := k; "
            "    Console.WriteLine(io.Peek().Name()); "
            "    Console.WriteLine(\"end\"); "
            "    return 0; "),
        &stdoutText);
    BEZEL_EXPECT_FALSE(r == -1);  // not a toolchain-skip / unrelated failure
    BEZEL_EXPECT_FALSE(r == -2);
    BEZEL_EXPECT_EQ(r, 0);
    BEZEL_EXPECT_EQ(stdoutText, std::string("dog\nend\n"));
}

// (2) ACCEPT — xmod: `var ia: IO<Animal> := k; ia.Peek().Name()`.
// Covariant `out T` binding. Same expected print: `dog` then `end`.
BEZEL_TEST(imported_generic_interface_method_return_t_4937,
           xmod_ia_Dot_Peek_Dot_Name_prints_dog_then_end_covariant_Animal) {
    std::string stdoutText;
    int r = compileImportRunCapture4937(
        kLib4937,
        kApp4937(
            "    using var k := new Ken4937(); "
            "    var ia: IO4937<An4937> := k; "
            "    Console.WriteLine(ia.Peek().Name()); "
            "    Console.WriteLine(\"end\"); "
            "    return 0; "),
        &stdoutText);
    BEZEL_EXPECT_FALSE(r == -1);
    BEZEL_EXPECT_FALSE(r == -2);
    BEZEL_EXPECT_EQ(r, 0);
    BEZEL_EXPECT_EQ(stdoutText, std::string("dog\nend\n"));
}

// (3) ACCEPT — xmod CONTROL: `k.Peek().Name()` class receiver. The
// issue itself notes already works (the Kennel's IO<Dog> base-chain
// substitution on a CLASS receiver was already correctly applied via
// the existing class-chain walker). Both pre- and post-fix ACCEPT
// and print `dog` then `end`.
BEZEL_TEST(imported_generic_interface_method_return_t_4937,
           xmod_k_Dot_Peek_Dot_Name_prints_dog_class_receiver_control) {
    std::string stdoutText;
    int r = compileImportRunCapture4937(
        kLib4937,
        kApp4937(
            "    using var k := new Ken4937(); "
            "    Console.WriteLine(k.Peek().Name()); "
            "    Console.WriteLine(\"end\"); "
            "    return 0; "),
        &stdoutText);
    BEZEL_EXPECT_FALSE(r == -1);
    BEZEL_EXPECT_FALSE(r == -2);
    BEZEL_EXPECT_EQ(r, 0);
    BEZEL_EXPECT_EQ(stdoutText, std::string("dog\nend\n"));
}

// (4) ACCEPT — single-module CONTROL: byte-identical logic with lib
// and app concatenated into `module M`. Pre- and post-fix both print
// `dog dog dog end` (4 WriteLines). Pins that single-module path stays
// intact and that the cross-module gap (rows 1-2) was specifically
// the import-side parent-param substitution gate, not a deeper
// sema or codegen regression. (One-module is built without a
// Dependency so it uses popen directly.)
BEZEL_TEST(imported_generic_interface_method_return_t_4937,
           single_module_twin_prints_dog_x3_end_control) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4937sm_");
    TempFile4937 sSrc((dir / (stem + ".bzl")).string());
    TempFile4937 sExe((dir / stem).string());
    writeSource4937(sSrc.path, kSingleModule4937);
    bezel::Driver driver;
    BEZEL_EXPECT_TRUE(driver.compileFile(sSrc.path, sExe.path).success);

    std::string cmd = sExe.path + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    BEZEL_EXPECT_TRUE(f != nullptr);
    std::array<char, 256> buf{};
    std::string captured;
    while (true) {
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        captured.append(buf.data(), n);
    }
    int rc = pclose(f);
    BEZEL_EXPECT_TRUE(WIFEXITED(rc));
    BEZEL_EXPECT_EQ(WEXITSTATUS(rc), 0);
    BEZEL_EXPECT_EQ(captured, std::string("dog\ndog\ndog\nend\n"));
}

}  // namespace bezel::module
