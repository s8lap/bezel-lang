// Cross-module — #2936 (grammar A.8 / §10.2, v0.240): the §10.2 explicit
// enum-value arm of the "one integer-literal parser" fix.
//
// The §15.1 inline-array and §17.14 constant-folding arms are pinned in
// tests/codegen/int_literal_one_parser_2936_test.cpp; the composition with
// #2898 is pinned in tests/module/xmod_generic_field_init_2898_test.cpp.
//
// #2936 — `tryEvalIntLiteral` decoded an explicit §10.2 enum value with
// `strtoll(text, &end, 0)`. Base 0 auto-detects `0x`, so hex crossed correctly,
// but grammar A.8 also admits `0b` binary literals and `_` digit separators in
// every base, and base 0 handles neither: `0b101` read as 0 and `1_0` as 1. The
// §10.2 continuation rule then numbered every following member from the wrong
// base. THIS is the arm a single-module enum never reaches — a same-module enum
// takes codegen's own (always-correct) `parseIntLiteral` — so the pin has to
// cross a real module boundary. The sibling single-module cases in
// tests/codegen/int_literal_one_parser_2936_test.cpp are labelled controls for
// exactly that reason.

#include "module/ModuleLoader.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a library object, then compile+link `appSrc` against it and
// run the result, returning the process exit code (or -1 if any step failed).
// Exit codes are 8-bit, so every expected value below is kept under 256.
int buildAndRun(const std::string& stem, const std::string& libSrc,
                const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile a((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile b((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(a.path, libSrc);
    writeSource(b.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(a.path, aObj.path);
    if (!ra.success) return -1;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(b.path, bExe.path, deps);
    if (!rb.success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// --- #2936: §10.2 explicit enum values across a module boundary -------------

// ⚠ ACCEPT-CONTROL, NOT a fix pin — and I predicted otherwise. MEASURED: this
// passes on the unfixed compiler too (with #2936 reverted and #2898 kept, the
// three cases here go 2 pass / 1 fail, and the one that fails is the underscore
// row below). So although `strtoll("0b101", &end, 0)` really does return 0 —
// base 0 recognises `0x` and leading-0 octal but not `0b` — an imported enum's
// explicit value evidently does not reach the importer through
// `tryEvalIntLiteral` on this path, and the binary spelling crosses correctly by
// some other route. The route was not identified; what is recorded here is the
// measurement, not a mechanism.
//
// Kept as a control: it pins that the binary spelling stays correct, and if a
// future change routes §10.2 values through the metadata text after all, this
// becomes a live regression test for free.
BEZEL_TEST(xmod_int_literal_2936, imported_enum_binary_value_control_spec_10_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2936_bin",
            "module ELib; public enum E A := 0b101, B, C; end E;",
            "module EApp; import ELib; class C "
            "  public static method Main(): int return int(E.B); end Main; "
            "end C;"),
        6);
}

// A.8 `_` digit separators — the ONE genuine fix pin in this file's §10.2
// group (measured: the only one of the three that fails with #2936 reverted).
// `P := 1_0` is 10, so Q is 11; the old base-0 parse stopped at the `_` and
// read 1, making Q = 2.
BEZEL_TEST(xmod_int_literal_2936, imported_enum_underscore_value_spec_10_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2936_us",
            "module ULib; public enum U P := 1_0, Q; end U;",
            "module UApp; import ULib; class C "
            "  public static method Main(): int return int(U.Q); end Main; "
            "end C;"),
        11);
}

// CONTROL: hex already crossed correctly (base-0 `strtoll` does detect `0x`),
// so this passes both with and without the fix. Kept to bound the claim — the
// defect was `0b` and `_`, not "non-decimal literals" in general — and to catch
// a fix that repaired those two while breaking the one base that worked.
BEZEL_TEST(xmod_int_literal_2936, imported_enum_hex_value_control_spec_10_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2936_hex",
            "module HLib; public enum H X := 0x10, Y; end H;",
            "module HApp; import HLib; class C "
            "  public static method Main(): int return int(H.Y); end Main; "
            "end C;"),
        17);
}

}  // namespace bezel::module
