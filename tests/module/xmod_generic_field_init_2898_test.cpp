// Cross-module — #2898 (§17.14 / §2.8.2 clause 2 / §22.13, v0.240): a folded
// compile-time constant must cross as an imported GENERIC class's instance
// field initializer.
//
// `renderConstExpr` (src/module/ModuleMetadataBuilder.cpp) had arms only for
// BARE literals, so a §17.14 compile-time constant written `:= 100 + 7`
// rendered "" and every consumer read that as "not a compile-time constant".
// For an imported generic's instance field that is a false REJECT: #2860's
// guard is category-blind (it asks whether a value reached the slot), so an
// empty rendering made it refuse a construction §2.8.2 clause 2 requires to
// behave exactly as the same-module one does — "An external instantiation
// behaves identically to a same-module instantiation of the same generic at the
// same arguments."
//
// The fix FOLDS such an expression to a literal, through the one §17.14 folder
// (`sema::foldIntConst`), rather than rendering it textually — `valueText` is
// consumed as TEXT by codegen (`constValues_`, ModuleLoader.cpp), so widening
// what it may contain would make every consumer an evaluator. Folding keeps it a
// literal, the shape they all already assume.
//
// ⚠ This fix DEPENDS on #2936 and had to land with it. #2898's fold runs through
// `sema::foldIntConst`, whose integer-literal arm was a base-10 `std::stoll`
// until #2936. MEASURED: with this fold present and #2936 reverted,
// `:= 0x60 + 11` crosses as **11** — a silent wrong value where a loud rejection
// stood before. That composition is the last case in this file.

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
// run the result, returning the process exit code (-1 if any step failed — which
// is how the #2898 false reject registers). Exit codes are 8-bit, so every
// expected value below is kept under 256.
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

// --- #2898: a folded §17.14 constant crosses as a generic's field init ------

// The headline: `:= 100 + 7` on an imported GENERIC class's instance field. The
// #2860 guard rejected the CONSTRUCTION because `renderConstExpr` returned "".
// -1 here means the compile failed, i.e. the false reject is back.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_constant_expression_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_add",
            "module BLib; public class Box<T> "
            "  public tag: int := 100 + 7; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Box;",
            "module BApp; import BLib; class C "
            "  public static method Main(): int "
            "    using var b := new Box<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// CONTROL: the bare-literal spelling already crossed (that is what #2860
// fixed), so this passes both ways. It is the reference the subject row must
// match — §2.8.2 clause 2 is precisely "the external instantiation behaves
// identically", so `:= 107` and `:= 100 + 7` must agree.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_bare_literal_control_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_lit",
            "module LLib; public class Lit<T> "
            "  public tag: int := 107; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Lit;",
            "module LApp; import LLib; class C "
            "  public static method Main(): int "
            "    using var b := new Lit<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// Operator coverage for the fold: subtraction, multiplication+addition,
// parenthesised division, shift+or, double negation, and `mod` — every one
// written to reach the same 107 so a wrong fold is a wrong exit code rather
// than a compile failure.
BEZEL_TEST(xmod_generic_field_init_2898, generic_field_init_operator_grid_spec_17_14) {
    const char* exprs[] = {"200 - 93", "10 * 10 + 7", "(100 + 14) / 2 + 50",
                           "1 << 6 | 43", "-(-107)", "106 mod 100 + 101"};
    for (const char* e : exprs) {
        std::string lib =
            std::string("module GLib; public class Ar<T> "
                        "  public tag: int := ") + e +
            "; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Ar;";
        BEZEL_EXPECT_EQ(
            buildAndRun("bezel_2898_grid",
                lib,
                "module GApp; import GLib; class C "
                "  public static method Main(): int "
                "    using var b := new Ar<int>(1); return b.tag; "
                "  end Main; end C;"),
            107);
    }
}

// ⚠ THE COMPOSITION, and the reason #2936 had to land WITH #2898: a hex operand
// inside the folded initializer. With #2898 alone, `sema::foldIntConst`'s
// base-10 literal arm read `0x60` as 0, so this would have crossed as 11 — a
// silent wrong value where a loud rejection stood before. Both fixes present:
// 107.
BEZEL_TEST(xmod_generic_field_init_2898, hex_operand_in_generic_field_initializer_spec_a8) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_hex",
            "module XLib; public class Hx<T> "
            "  public tag: int := 0x60 + 11; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Hx;",
            "module XApp; import XLib; class C "
            "  public static method Main(): int "
            "    using var b := new Hx<int>(1); return b.tag; "
            "  end Main; end C;"),
        107);
}

// --- OVER-ACCEPTANCE controls: what must STILL be rejected ------------------
//
// #2898 widens what crosses the artifact boundary, so the guard it relaxes must
// be shown still to fire. Both of these are §2.8.2 rejections that must survive:
// the fix folds only LITERAL arithmetic, and its identifier resolver answers
// "not a constant" for every name deliberately — `renderConstExpr` renders a
// bare identifier as its NAME, which has no guaranteed importer-side meaning, so
// resolving names here would let a same-named-but-different const on the
// importing side silently supply a value. A loud rejection is the correct
// outcome for anything the fold cannot reach; #2874 covers carrying genuinely
// non-constant initializers.

// A call in the initializer is not a §17.14 constant expression: still rejected.
BEZEL_TEST(xmod_generic_field_init_2898, non_constant_initializer_still_rejected_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_rej",
            "module SLib; public class St<T> "
            "  public tag: int := 100 + \"abcdefg\".Length; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end St;",
            "module SApp; import SLib; class C "
            "  public static method Main(): int "
            "    using var b := new St<int>(1); return b.tag; "
            "  end Main; end C;"),
        -1);
}

// A FLOAT constant expression: `foldIntConst` is integer-only by construction,
// so this still renders "" and is still rejected. Pins that the fix did not
// quietly widen to a domain it cannot evaluate.
BEZEL_TEST(xmod_generic_field_init_2898, float_constant_initializer_still_rejected_spec_2_8_2) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_2898_dbl",
            "module DLib; public class Db<T> "
            "  public tag: double := 1.5 + 0.5; public v: T; "
            "  public constructor(x: T) v := x; end constructor; end Db;",
            "module DApp; import DLib; class C "
            "  public static method Main(): int "
            "    using var b := new Db<int>(1); return 0; "
            "  end Main; end C;"),
        -1);
}

}  // namespace bezel::module
